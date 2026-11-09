//! A read-only comparison panel. One worker does disk I/O, decode, tonemapping and metrics; generations
//! discard stale results when the target or selected capture changes. No app settings or game writes.
use crate::i18n::{Lang, t};
use egui::{Color32, ColorImage, Pos2, Rect, Sense, TextureHandle, TextureOptions, Vec2};
use retoque_data::captures::{self, Pair, Stats};
use std::{
    collections::{HashMap, VecDeque},
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    time::{Duration, Instant, SystemTime},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CompareMode {
    #[default]
    Wipe,
    SideBySide,
    Flip,
    Difference,
}
impl CompareMode {
    fn key(self) -> &'static str {
        match self {
            Self::Wipe => "wipe",
            Self::SideBySide => "side",
            Self::Flip => "flip",
            Self::Difference => "diff",
        }
    }
}
#[derive(Clone, Copy, Debug)]
struct Transform {
    scale: f32,
    pan: Vec2,
    fit: bool,
}
impl Default for Transform {
    fn default() -> Self {
        Self {
            scale: 1.0,
            pan: Vec2::ZERO,
            fit: true,
        }
    }
}
impl Transform {
    fn zoom_at(&mut self, factor: f32, cursor: Vec2) {
        let next = (self.scale * factor).clamp(0.01, 4096.0);
        self.pan = cursor - (cursor - self.pan) * (next / self.scale);
        self.scale = next;
        self.fit = false;
    }
}
struct Loaded {
    original: TextureHandle,
    processed: TextureHandle,
    diff: TextureHandle,
    size: Vec2,
    stats: Stats,
    exposure: f32,
    elapsed: Duration,
    ev: f32,
}
struct Prepared {
    images: [ColorImage; 3],
    thumbnail: ColorImage,
    stats: Stats,
    exposure: f32,
    elapsed: Duration,
    ev: f32,
}
enum WorkKind {
    Scan(PathBuf),
    Load(u64, Pair, f32),
    Thumbnail(usize, Pair),
}
struct Work {
    epoch: u64,
    kind: WorkKind,
}
enum Reply {
    Found(u64, Result<(Vec<Pair>, captures::CaptureHelp), ()>),
    Loaded(u64, u64, Result<Box<Prepared>, ()>),
    Thumbnail(u64, usize, Result<ColorImage, ()>),
}
struct Worker {
    tx: mpsc::Sender<Work>,
    rx: mpsc::Receiver<Reply>,
    epoch: Arc<AtomicU64>,
    request: Arc<AtomicU64>,
}
fn color(image: &captures::RgbaImage) -> ColorImage {
    ColorImage::from_rgba_unmultiplied([image.width() as usize, image.height() as usize], image.as_raw())
}
fn thumbnail(image: &captures::RgbaImage) -> ColorImage {
    let ratio = (168.0 / image.width() as f32)
        .min(80.0 / image.height() as f32)
        .min(1.0);
    color(&image::imageops::thumbnail(
        image,
        (image.width() as f32 * ratio).round().max(1.0) as u32,
        (image.height() as f32 * ratio).round().max(1.0) as u32,
    ))
}
fn prepare(pair: &Pair, ev: f32) -> std::io::Result<Prepared> {
    let start = Instant::now();
    let pair = captures::load_pair_with_ev(pair, ev)?;
    let stats = captures::stats(&pair.original, &pair.processed)?;
    let diff = captures::diff_image(&pair.original, &pair.processed)?;
    Ok(Prepared {
        thumbnail: thumbnail(&pair.original),
        images: [color(&pair.original), color(&pair.processed), color(&diff)],
        stats,
        exposure: pair.display.exposure,
        elapsed: start.elapsed(),
        ev,
    })
}
#[derive(PartialEq)]
struct ThumbnailStamp {
    bytes: u64,
    modified: Option<SystemTime>,
    provenance: Vec<(String, String)>,
}
#[derive(Default)]
struct ThumbnailCache {
    images: HashMap<PathBuf, (ThumbnailStamp, ColorImage)>,
}
impl ThumbnailCache {
    fn get(&mut self, pair: &Pair) -> std::io::Result<ColorImage> {
        let metadata = std::fs::metadata(&pair.original)?;
        let stamp = ThumbnailStamp {
            bytes: metadata.len(),
            modified: metadata.modified().ok(),
            provenance: pair.provenance.clone(),
        };
        if let Some((old, image)) = self.images.get(&pair.original)
            && old == &stamp
        {
            return Ok(image.clone());
        }
        let image = color(&captures::load_thumbnail(pair)?);
        // Only small decoded previews survive between targets; never retain full-resolution pairs here.
        if self.images.len() >= 256 {
            self.images.clear();
        }
        self.images.insert(pair.original.clone(), (stamp, image.clone()));
        Ok(image)
    }
}
impl Worker {
    fn new(ctx: egui::Context) -> Self {
        let (tx, jobs) = mpsc::channel::<Work>();
        // A bounded return queue prevents stale decoded pairs accumulating when the panel is hidden.
        let (results, rx) = mpsc::sync_channel(2);
        let epoch = Arc::new(AtomicU64::new(0));
        let current = epoch.clone();
        let request = Arc::new(AtomicU64::new(0));
        let current_request = request.clone();
        std::thread::spawn(move || {
            let mut queue = VecDeque::<Work>::new();
            let mut thumbnails = ThumbnailCache::default();
            loop {
                if queue.is_empty() {
                    let Ok(job) = jobs.recv() else { break };
                    queue.push_back(job);
                }
                queue.extend(jobs.try_iter());
                queue.retain(|job| {
                    job.epoch == current.load(Ordering::Relaxed)
                        && match &job.kind {
                            WorkKind::Load(id, _, _) => *id == current_request.load(Ordering::Relaxed),
                            _ => true,
                        }
                });
                // Target scans and the current selection take precedence over background thumbnails.
                let next = queue
                    .iter()
                    .position(|job| matches!(job.kind, WorkKind::Scan(_)))
                    .or_else(|| queue.iter().position(|job| matches!(job.kind, WorkKind::Load(..))))
                    .unwrap_or(0);
                let Some(job) = queue.remove(next) else { continue };
                let reply = match job.kind {
                    WorkKind::Scan(path) => Reply::Found(
                        job.epoch,
                        captures::scan(&path)
                            .map(|pairs| (pairs, captures::capture_help(&path)))
                            .map_err(|_| ()),
                    ),
                    WorkKind::Load(id, pair, ev) => {
                        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                            prepare(&pair, ev).map(Box::new).map_err(|_| ())
                        }))
                        .unwrap_or(Err(()));
                        if id != current_request.load(Ordering::Relaxed) {
                            continue;
                        }
                        Reply::Loaded(job.epoch, id, result)
                    }
                    WorkKind::Thumbnail(index, pair) => Reply::Thumbnail(
                        job.epoch,
                        index,
                        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                            thumbnails.get(&pair).map_err(|_| ())
                        }))
                        .unwrap_or(Err(())),
                    ),
                };
                if job.epoch != current.load(Ordering::Relaxed) {
                    continue;
                }
                if results.send(reply).is_err() {
                    break;
                }
                ctx.request_repaint();
            }
        });
        Self { tx, rx, epoch, request }
    }
}

pub struct CompareView {
    target: Option<PathBuf>,
    worker: Option<Worker>,
    epoch: u64,
    request: u64,
    pairs: Vec<Pair>,
    thumbnails: Vec<Option<TextureHandle>>,
    thumbnail_failed: Vec<bool>,
    exposure_ev: f32,
    selected: Option<usize>,
    loaded: Option<Loaded>,
    scanning: bool,
    loading: bool,
    error: Option<&'static str>,
    mode: CompareMode,
    transform: Transform,
    divider: f32,
    dragging_divider: bool,
    details_open: bool,
    expanded: bool,
    restore_expand_focus: bool,
    capture_help: captures::CaptureHelp,
}
impl Default for CompareView {
    fn default() -> Self {
        Self {
            target: None,
            worker: None,
            epoch: 0,
            request: 0,
            pairs: vec![],
            thumbnails: vec![],
            thumbnail_failed: vec![],
            exposure_ev: 0.0,
            selected: None,
            loaded: None,
            scanning: false,
            loading: false,
            error: None,
            mode: CompareMode::Wipe,
            transform: Transform::default(),
            divider: 0.5,
            dragging_divider: false,
            details_open: false,
            expanded: false,
            restore_expand_focus: false,
            capture_help: captures::CaptureHelp::default(),
        }
    }
}
impl CompareView {
    pub fn new() -> Self {
        Self {
            divider: 0.5,
            ..Self::default()
        }
    }
    pub fn set_mode(&mut self, mode: CompareMode) {
        if mode != self.mode {
            self.mode = mode;
            self.transform = Transform::default();
        }
    }
    pub fn mode(&self) -> CompareMode {
        self.mode
    }
    /// CPU decode + display conversion + stats; excludes GPU texture upload.
    pub fn last_load_time(&self) -> Option<Duration> {
        if self.loading {
            None
        } else {
            self.loaded.as_ref().map(|p| p.elapsed)
        }
    }
    pub fn thumbnail_progress(&self) -> (usize, usize) {
        (
            self.thumbnails.iter().filter(|image| image.is_some()).count(),
            self.pairs.len(),
        )
    }
    pub fn exposure_ev(&self) -> f32 {
        self.exposure_ev
    }
    /// Relative HDR exposure, applied to both images. The worker coalesces obsolete requests.
    pub fn set_exposure_ev(&mut self, ev: f32) {
        if !ev.is_finite() {
            return;
        }
        let ev = ev.clamp(-4.0, 4.0);
        if self.exposure_ev == ev {
            return;
        }
        self.exposure_ev = ev;
        if let Some(index) = self.selected
            && self.pairs[index].hdr
        {
            self.request_load(index);
        }
    }
    fn request_load(&mut self, index: usize) {
        self.request += 1;
        self.loading = true;
        self.error = None;
        if let Some(worker) = &self.worker {
            worker.request.store(self.request, Ordering::Relaxed);
            let _ = worker.tx.send(Work {
                epoch: self.epoch,
                kind: WorkKind::Load(self.request, self.pairs[index].clone(), self.exposure_ev),
            });
        }
    }
    pub(crate) fn invalidate_captures(&mut self, target: &Path) {
        if self.target.as_deref() == Some(target) {
            self.scan(target);
        }
    }
    fn scan(&mut self, target: &Path) {
        self.epoch += 1;
        self.request += 1;
        self.target = Some(target.to_owned());
        self.pairs.clear();
        self.thumbnails.clear();
        self.thumbnail_failed.clear();
        self.exposure_ev = 0.0;
        self.loaded = None;
        self.selected = None;
        self.scanning = true;
        self.loading = false;
        self.error = None;
        self.transform = Transform::default();
        self.divider = 0.5;
        if let Some(worker) = &self.worker {
            worker.epoch.store(self.epoch, Ordering::Relaxed);
            worker.request.store(self.request, Ordering::Relaxed);
            let _ = worker.tx.send(Work {
                epoch: self.epoch,
                kind: WorkKind::Scan(target.to_owned()),
            });
        }
    }
    fn select(&mut self, index: usize) {
        self.selected = Some(index);
        self.loaded = None;
        self.exposure_ev = 0.0;
        self.transform = Transform::default();
        self.request_load(index);
    }
    fn receive(&mut self, ctx: &egui::Context) {
        for _ in 0..4 {
            let Some(reply) = self.worker.as_ref().and_then(|w| w.rx.try_recv().ok()) else {
                break;
            };
            match reply {
                Reply::Found(epoch, result) if epoch == self.epoch => {
                    self.scanning = false;
                    match result {
                        Ok((pairs, help)) => {
                            self.capture_help = help;
                            self.pairs = pairs;
                            self.thumbnails = vec![None; self.pairs.len()];
                            self.thumbnail_failed = vec![false; self.pairs.len()];
                            if !self.pairs.is_empty() {
                                self.select(0);
                                for (index, pair) in self.pairs.iter().enumerate() {
                                    let _ = self.worker.as_ref().unwrap().tx.send(Work {
                                        epoch,
                                        kind: WorkKind::Thumbnail(index, pair.clone()),
                                    });
                                }
                            }
                        }
                        Err(()) => self.error = Some("scan_error"),
                    }
                }
                Reply::Loaded(epoch, id, result) if epoch == self.epoch && id == self.request => {
                    self.loading = false;
                    match result {
                        Ok(result) => {
                            let Prepared {
                                images: [original, processed, diff],
                                thumbnail,
                                stats,
                                exposure,
                                elapsed,
                                ev,
                            } = *result;
                            let size = Vec2::new(original.size[0] as f32, original.size[1] as f32);
                            let options = TextureOptions::LINEAR;
                            self.loaded = Some(Loaded {
                                original: ctx.load_texture("compare-original", original, options),
                                processed: ctx.load_texture("compare-processed", processed, options),
                                diff: ctx.load_texture("compare-diff", diff, options),
                                size,
                                stats,
                                exposure,
                                elapsed,
                                ev,
                            });
                            if ev == 0.0
                                && let Some(index) = self.selected
                                && self.thumbnails[index].is_none()
                            {
                                self.thumbnails[index] = Some(ctx.load_texture("compare-thumb", thumbnail, options));
                            }
                        }
                        Err(()) => self.error = Some("load_error"),
                    }
                }
                Reply::Thumbnail(epoch, index, image) if epoch == self.epoch && index < self.thumbnails.len() => {
                    match image {
                        Ok(image) => {
                            self.thumbnails[index] =
                                Some(ctx.load_texture("compare-thumb", image, TextureOptions::LINEAR))
                        }
                        Err(()) => self.thumbnail_failed[index] = true,
                    }
                }
                _ => {}
            }
        }
    }
    pub fn ui(&mut self, ui: &mut egui::Ui, lang: Lang, target: &Path) {
        if self.worker.is_none() {
            self.worker = Some(Worker::new(ui.ctx().clone()));
        }
        if self.target.as_deref() != Some(target) {
            self.scan(target);
        }
        self.receive(ui.ctx());
        if self.expanded && !self.pairs.is_empty() {
            let ctx = ui.ctx().clone();
            let size = (ctx.content_rect().size() - Vec2::splat(48.0)).max(Vec2::new(280.0, 240.0));
            let modal = egui::Modal::new(ui.scope_id().with("compare-expanded"))
                .frame(egui::Frame::popup(ui.style()).inner_margin(12.0))
                .show(&ctx, |ui| {
                    ui.set_width(size.x);
                    ui.set_min_height(size.y);
                    ui.set_max_height(size.y);
                    self.content(ui, lang, target);
                });
            if modal.should_close() {
                self.expanded = false;
                self.restore_expand_focus = true;
            }
        } else {
            self.content(ui, lang, target);
        }
    }
    fn content(&mut self, ui: &mut egui::Ui, lang: Lang, target: &Path) {
        if let Some(error) = self.error {
            ui.colored_label(Color32::from_rgb(238, 112, 112), t(lang, error));
        }
        if self.pairs.is_empty() {
            if self.scanning {
                ui.spinner();
                ui.label(t(lang, "scanning"));
            } else {
                self.empty(ui, lang, target);
            }
            return;
        }
        let held = self.toolbar(ui, lang, target);
        ui.add_space(6.0);
        // Only the filmstrip and its compact footer are reserved. Metadata has no cost while closed.
        let drawer_height = if self.details_open {
            (ui.available_height() * 0.30).clamp(100.0, 220.0)
        } else {
            0.0
        };
        let canvas_height = (ui.available_height() - 126.0 - drawer_height).max(80.0);
        if self.loaded.is_some() {
            self.canvas(ui, lang, Vec2::new(ui.available_width(), canvas_height), held);
        } else {
            egui::Frame::new()
                .fill(Color32::from_rgb(9, 11, 14))
                .corner_radius(8.0)
                .show(ui, |ui| {
                    ui.set_min_size(Vec2::new(ui.available_width(), canvas_height));
                    ui.centered_and_justified(|ui| {
                        ui.spinner();
                        ui.label(t(lang, "loading"));
                    });
                });
        }
        ui.add_space(6.0);
        self.filmstrip(ui, lang);
        ui.horizontal(|ui| {
            ui.small(format!("{} ({})", t(lang, "captures"), self.pairs.len()));
            if let Some(index) = self.selected {
                ui.add(egui::Label::new(egui::RichText::new(&self.pairs[index].stem).small()).truncate())
                    .on_hover_text(&self.pairs[index].stem);
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.toggle_value(&mut self.details_open, t(lang, "cmp.details"));
            });
        });
        if self.details_open {
            egui::Frame::new()
                .fill(ui.visuals().faint_bg_color)
                .inner_margin(10.0)
                .corner_radius(8.0)
                .show(ui, |ui| {
                    egui::ScrollArea::vertical()
                        .id_salt("compare-details")
                        .max_height(drawer_height)
                        .show(ui, |ui| self.details(ui, lang));
                });
        }
    }
    fn empty(&mut self, ui: &mut egui::Ui, lang: Lang, target: &Path) {
        ui.add_space(24.0);
        ui.heading(t(lang, "cmp.empty.title"));
        ui.add_space(12.0);
        if self.capture_help.configuration == Some(false) {
            ui.label(t(lang, "cmp.empty.install").replace("{tab}", t(lang, "tab.install")));
            ui.add_space(12.0);
        }
        let menu = if self.capture_help.menu_disabled {
            t(lang, "cmp.empty.disabled").to_owned()
        } else if self.capture_help.shortcut_disabled {
            t(lang, "cmp.empty.no_shortcut").to_owned()
        } else if let Some(key) = &self.capture_help.menu_key {
            t(lang, "cmp.empty.menu").replace("{key}", key)
        } else {
            t(lang, "cmp.empty.custom").to_owned()
        };
        for (index, text) in [t(lang, "cmp.empty.run"), menu.as_str(), t(lang, "cmp.empty.capture")]
            .into_iter()
            .enumerate()
        {
            ui.horizontal_top(|ui| {
                ui.label(
                    egui::RichText::new(format!("{}", index + 1))
                        .strong()
                        .color(Color32::from_rgb(240, 168, 69)),
                );
                ui.label(text);
            });
            ui.add_space(8.0);
        }
        if ui.button(t(lang, "cmp.empty.refresh")).clicked() {
            self.scan(target);
        }
    }
    fn toolbar(&mut self, ui: &mut egui::Ui, lang: Lang, target: &Path) -> bool {
        let compact = ui.available_width() < 850.0;
        let loaded = self.loaded.is_some();
        let mut held = false;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 5.0;
            ui.spacing_mut().button_padding = Vec2::new(7.0, 4.0);
            for mode in [
                CompareMode::Wipe,
                CompareMode::SideBySide,
                CompareMode::Flip,
                CompareMode::Difference,
            ] {
                let response = mode_button(ui, lang, mode, self.mode == mode, compact);
                if response.clicked() {
                    self.set_mode(mode);
                }
            }
            ui.add_enabled_ui(loaded, |ui| {
                if !(compact && self.mode == CompareMode::Flip) {
                    let fit = ui.button(t(lang, "cmp.fit.short")).on_hover_text(t(lang, "fit"));
                    fit.widget_info(|| {
                        egui::WidgetInfo::labeled(egui::accesskit::Role::Button, ui.is_enabled(), t(lang, "fit"))
                    });
                    if fit.clicked() {
                        self.transform = Transform::default();
                    }
                    if ui.button(t(lang, "one")).clicked() {
                        self.one_to_one(ui.ctx());
                    }
                }
                if self.mode == CompareMode::Flip {
                    let hold = ui.button(t(lang, "original")).on_hover_text(t(lang, "hold"));
                    hold.widget_info(|| {
                        egui::WidgetInfo::labeled(egui::accesskit::Role::Button, ui.is_enabled(), t(lang, "hold"))
                    });
                    held = hold.is_pointer_button_down_on();
                }
                if self.selected.is_some_and(|index| self.pairs[index].hdr) {
                    ui.spacing_mut().slider_width = if compact { 70.0 } else { 100.0 };
                    let mut ev = self.exposure_ev;
                    let slider = ui
                        .add(
                            egui::Slider::new(&mut ev, -4.0..=4.0)
                                .step_by(0.1)
                                .fixed_decimals(1)
                                .suffix(" EV")
                                .text(if compact { "" } else { t(lang, "exposure") }),
                        )
                        .on_hover_text(t(lang, "exposure_help"));
                    slider.widget_info(|| {
                        egui::WidgetInfo::labeled(egui::accesskit::Role::Slider, ui.is_enabled(), t(lang, "exposure"))
                    });
                    let mut apply = slider.drag_stopped() || (slider.changed() && !slider.dragged());
                    let reset = ui
                        .button(t(lang, "cmp.reset.short"))
                        .on_hover_text(t(lang, "exposure_reset"));
                    reset.widget_info(|| {
                        egui::WidgetInfo::labeled(
                            egui::accesskit::Role::Button,
                            ui.is_enabled(),
                            t(lang, "exposure_reset"),
                        )
                    });
                    if reset.clicked() {
                        ev = 0.0;
                        apply = true;
                    }
                    self.exposure_ev = ev;
                    if apply
                        && (self.loading || self.loaded.as_ref().is_some_and(|image| image.ev != ev))
                        && let Some(index) = self.selected
                    {
                        self.request_load(index);
                    }
                }
            });
            let tools = ui.menu_button(t(lang, "cmp.more"), |ui| {
                if ui.button(t(lang, "refresh")).clicked() {
                    self.scan(target);
                    ui.close();
                }
                ui.add_enabled_ui(loaded, |ui| {
                    if ui.button(t(lang, "fit")).clicked() {
                        self.transform = Transform::default();
                        ui.close();
                    }
                    if ui.button(t(lang, "one")).clicked() {
                        self.one_to_one(ui.ctx());
                        ui.close();
                    }
                    ui.horizontal(|ui| {
                        if ui.button(t(lang, "cmp.zoom_out")).clicked() {
                            self.transform.zoom_at(1.0 / 1.25, Vec2::ZERO);
                        }
                        if ui.button(t(lang, "cmp.zoom_in")).clicked() {
                            self.transform.zoom_at(1.25, Vec2::ZERO);
                        }
                    });
                    if self.mode == CompareMode::Wipe {
                        ui.add(
                            egui::Slider::new(&mut self.divider, 0.0..=1.0)
                                .show_value(false)
                                .text(t(lang, "cmp.divider")),
                        );
                    }
                });
                ui.separator();
                ui.strong(t(lang, "cmp.shortcuts"));
                ui.set_max_width(330.0);
                ui.label(t(lang, "cmp.keyboard"));
                if self.selected.is_some_and(|index| self.pairs[index].hdr) {
                    ui.separator();
                    ui.label(t(lang, "hdr_note"));
                }
            });
            tools.response.widget_info(|| {
                egui::WidgetInfo::labeled(egui::accesskit::Role::Button, ui.is_enabled(), t(lang, "cmp.tools"))
            });
            tools.response.on_hover_text(t(lang, "cmp.tools"));
            let expand = icon_button(
                ui,
                t(lang, if self.expanded { "cmp.close" } else { "cmp.expand" }),
                false,
            );
            paint_expand(ui, &expand, self.expanded);
            if self.restore_expand_focus && !self.expanded {
                expand.request_focus();
                self.restore_expand_focus = false;
            }
            if expand.clicked() {
                self.expanded = !self.expanded;
                self.restore_expand_focus = !self.expanded;
            }
            if self.loading {
                ui.spinner().on_hover_text(t(lang, "exposure_updating"));
            }
        });
        held
    }
    fn one_to_one(&mut self, ctx: &egui::Context) {
        self.transform = Transform {
            scale: 1.0 / ctx.pixels_per_point(),
            pan: Vec2::ZERO,
            fit: false,
        };
    }
    fn filmstrip(&mut self, ui: &mut egui::Ui, lang: Lang) {
        let mut selected = None;
        egui::ScrollArea::horizontal()
            .id_salt("compare-filmstrip")
            .max_height(82.0)
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    for (index, pair) in self.pairs.iter().enumerate() {
                        let (rect, response) = ui.allocate_exact_size(Vec2::new(132.0, 76.0), Sense::click());
                        response.widget_info(|| {
                            egui::WidgetInfo::selected(
                                egui::accesskit::Role::Button,
                                ui.is_enabled(),
                                self.selected == Some(index),
                                &pair.stem,
                            )
                        });
                        let painter = ui.painter_at(rect);
                        painter.rect_filled(rect, 6.0, ui.visuals().faint_bg_color);
                        if let Some(texture) = &self.thumbnails[index] {
                            let source = texture.size_vec2();
                            let scale = (124.0 / source.x).min(68.0 / source.y);
                            painter.image(
                                texture.id(),
                                Rect::from_center_size(rect.center(), source * scale),
                                Rect::from_min_max(Pos2::ZERO, Pos2::new(1.0, 1.0)),
                                Color32::WHITE,
                            );
                        } else {
                            painter.text(
                                rect.center(),
                                egui::Align2::CENTER_CENTER,
                                t(
                                    lang,
                                    if self.thumbnail_failed[index] {
                                        "thumbnail_error"
                                    } else {
                                        "loading"
                                    },
                                ),
                                egui::FontId::proportional(11.0),
                                ui.visuals().text_color(),
                            );
                        }
                        if self.selected == Some(index) || response.has_focus() {
                            painter.rect_stroke(
                                rect.shrink(1.0),
                                6.0,
                                (
                                    if response.has_focus() { 2.0 } else { 1.0 },
                                    if response.has_focus() {
                                        Color32::WHITE
                                    } else {
                                        Color32::from_rgb(240, 168, 69)
                                    },
                                ),
                                egui::StrokeKind::Inside,
                            );
                        }
                        if response.clicked() {
                            selected = Some(index);
                        }
                        response.on_hover_text(&pair.stem);
                    }
                });
            });
        if let Some(index) = selected {
            self.select(index);
        }
    }
    fn details(&self, ui: &mut egui::Ui, lang: Lang) {
        let Some(loaded) = &self.loaded else { return };
        ui.horizontal_wrapped(|ui| {
            ui.strong(t(lang, "stats"));
            ui.label(format!("{} {:.2} dB", t(lang, "psnr"), loaded.stats.psnr))
                .on_hover_text(t(lang, "stats_help"));
            ui.label(format!("{} {:.2}%", t(lang, "changed"), loaded.stats.changed_percent));
            ui.label(format!(
                "{} {:.3}%",
                t(lang, "mad"),
                loaded.stats.mean_absolute_difference * 100.0
            ));
            ui.small(format!("{} {:.2} s", t(lang, "loaded"), loaded.elapsed.as_secs_f64()));
        });
        if let Some(pair) = self.selected.and_then(|index| self.pairs.get(index)) {
            if pair.hdr {
                ui.small(format!(
                    "{} {:.3}× ({:+.1} EV)",
                    t(lang, "hdr"),
                    loaded.exposure,
                    loaded.ev
                ));
                ui.small(t(lang, "hdr_note"));
            } else {
                ui.small(t(lang, "sdr"));
            }
            if self.mode == CompareMode::Difference {
                ui.small(t(lang, "diff_key"));
            }
            ui.separator();
            provenance(ui, lang, pair);
        }
    }
    fn canvas(&mut self, ui: &mut egui::Ui, lang: Lang, size: Vec2, held: bool) {
        let (rect, response) = ui.allocate_exact_size(size, Sense::click_and_drag());
        response.widget_info(|| {
            egui::WidgetInfo::labeled(egui::accesskit::Role::Image, ui.is_enabled(), t(lang, "canvas"))
        });
        if response.clicked() || response.drag_started() {
            response.request_focus();
        }
        let active = response.has_focus() && !ui.ctx().text_edit_focused();
        if active {
            self.keyboard(ui);
        }
        let held =
            held || (ui.is_enabled() && !ui.ctx().text_edit_focused() && ui.input(|i| i.key_down(egui::Key::Space)));
        let loaded = self.loaded.as_ref().unwrap();
        let panes = if self.mode == CompareMode::SideBySide {
            [
                Rect::from_min_max(rect.min, Pos2::new(rect.center().x - 4.0, rect.bottom())),
                Rect::from_min_max(Pos2::new(rect.center().x + 4.0, rect.top()), rect.max),
            ]
        } else {
            [rect, rect]
        };
        if self.transform.fit {
            self.transform.scale = (panes[0].width() / loaded.size.x)
                .min(panes[0].height() / loaded.size.y)
                .max(0.01);
            self.transform.pan = Vec2::ZERO;
        }
        let pointer = response
            .interact_pointer_pos()
            .or_else(|| ui.input(|i| i.pointer.hover_pos()));
        let divider_x = rect.left() + rect.width() * self.divider;
        if response.drag_started() {
            // egui starts a drag only after crossing its threshold: use the press location, not
            // the already moved pointer, to decide whether the user grabbed the divider.
            self.dragging_divider = self.mode == CompareMode::Wipe
                && ui
                    .input(|i| i.pointer.press_origin())
                    .is_some_and(|p| (p.x - divider_x).abs() < 14.0);
        }
        if response.dragged() {
            if self.dragging_divider {
                if let Some(p) = pointer {
                    self.divider = ((p.x - rect.left()) / rect.width()).clamp(0.0, 1.0);
                }
            } else {
                self.transform.pan += response.drag_delta();
                self.transform.fit = false;
            }
        }
        if response.drag_stopped() {
            self.dragging_divider = false;
        }
        if response.hovered()
            && let Some(p) = pointer
        {
            let pane = if self.mode == CompareMode::SideBySide && p.x > rect.center().x {
                panes[1]
            } else {
                panes[0]
            };
            let (scroll, pinch) = ui.input(|i| (i.smooth_scroll_delta.y, i.zoom_delta()));
            if scroll != 0.0 || pinch != 1.0 {
                self.transform
                    .zoom_at((scroll * 0.002).exp() * pinch, p - pane.center());
                ui.input_mut(|i| i.smooth_scroll_delta = Vec2::ZERO);
            }
            ui.ctx()
                .set_cursor_icon(if self.mode == CompareMode::Wipe && (p.x - divider_x).abs() < 14.0 {
                    egui::CursorIcon::ResizeHorizontal
                } else {
                    egui::CursorIcon::Grab
                });
        }
        let painter = ui.painter_at(rect);
        painter.rect_filled(rect, 4.0, Color32::from_rgb(9, 11, 14));
        let draw = |texture: &TextureHandle, pane: Rect, clip: Rect| {
            let destination =
                Rect::from_center_size(pane.center() + self.transform.pan, loaded.size * self.transform.scale);
            painter.with_clip_rect(clip.intersect(rect)).image(
                texture.id(),
                destination,
                Rect::from_min_max(Pos2::ZERO, Pos2::new(1.0, 1.0)),
                Color32::WHITE,
            );
        };
        let shown_mode = if held { CompareMode::Flip } else { self.mode };
        match shown_mode {
            CompareMode::Wipe => {
                let x = rect.left() + rect.width() * self.divider;
                draw(
                    &loaded.original,
                    rect,
                    Rect::from_min_max(rect.min, Pos2::new(x, rect.bottom())),
                );
                draw(
                    &loaded.processed,
                    rect,
                    Rect::from_min_max(Pos2::new(x, rect.top()), rect.max),
                );
                painter.line_segment(
                    [Pos2::new(x, rect.top()), Pos2::new(x, rect.bottom())],
                    (2.0, Color32::WHITE),
                );
                painter.circle_filled(Pos2::new(x, rect.center().y), 13.0, Color32::from_rgb(240, 168, 69));
                // Geometry keeps the handle legible with both the system and egui fallback fonts.
                let center = Pos2::new(x, rect.center().y);
                for sign in [-1.0, 1.0] {
                    let tip = center + Vec2::new(sign * 7.0, 0.0);
                    for y in [-4.0, 4.0] {
                        painter.line_segment([tip, center + Vec2::new(sign * 3.0, y)], (1.5, Color32::BLACK));
                    }
                }
                image_label(
                    &painter,
                    rect.left_top() + Vec2::splat(12.0),
                    t(lang, "original"),
                    false,
                );
                image_label(
                    &painter,
                    rect.right_top() + Vec2::new(-12.0, 12.0),
                    t(lang, "processed"),
                    true,
                );
            }
            CompareMode::SideBySide => {
                draw(&loaded.original, panes[0], panes[0]);
                draw(&loaded.processed, panes[1], panes[1]);
                image_label(
                    &painter,
                    panes[0].left_top() + Vec2::splat(12.0),
                    t(lang, "original"),
                    false,
                );
                image_label(
                    &painter,
                    panes[1].left_top() + Vec2::splat(12.0),
                    t(lang, "processed"),
                    false,
                );
            }
            CompareMode::Flip => {
                draw(if held { &loaded.original } else { &loaded.processed }, rect, rect);
                image_label(
                    &painter,
                    rect.left_top() + Vec2::splat(12.0),
                    t(lang, if held { "original" } else { "processed" }),
                    false,
                );
            }
            CompareMode::Difference => {
                draw(&loaded.diff, rect, rect);
                image_label(&painter, rect.left_top() + Vec2::splat(12.0), t(lang, "diff"), false);
            }
        }
        if self.selected.is_some_and(|index| self.pairs[index].hdr) {
            image_label(
                &painter,
                rect.left_bottom() + Vec2::new(12.0, -24.0),
                &format!("{} · {:+.1} EV", t(lang, "cmp.preview"), loaded.ev),
                false,
            );
        }
        if response.has_focus() {
            painter.rect_stroke(rect.shrink(1.0), 4.0, (2.0, Color32::WHITE), egui::StrokeKind::Inside);
        }
    }
    fn keyboard(&mut self, ui: &mut egui::Ui) {
        for (key, mode) in [
            (egui::Key::Num1, CompareMode::Wipe),
            (egui::Key::Num2, CompareMode::SideBySide),
            (egui::Key::Num3, CompareMode::Flip),
            (egui::Key::Num4, CompareMode::Difference),
        ] {
            if ui.input_mut(|i| i.consume_key(egui::Modifiers::NONE, key)) {
                self.set_mode(mode);
            }
        }
        if ui.input_mut(|i| i.consume_key(egui::Modifiers::NONE, egui::Key::F)) {
            self.transform = Transform::default();
        }
        for (key, delta) in [
            (egui::Key::ArrowLeft, Vec2::new(-24.0, 0.0)),
            (egui::Key::ArrowRight, Vec2::new(24.0, 0.0)),
            (egui::Key::ArrowUp, Vec2::new(0.0, -24.0)),
            (egui::Key::ArrowDown, Vec2::new(0.0, 24.0)),
        ] {
            if self.mode == CompareMode::Wipe
                && delta.x != 0.0
                && ui.input_mut(|i| i.consume_key(egui::Modifiers::SHIFT, key))
            {
                self.divider = (self.divider + delta.x.signum() * 0.05).clamp(0.0, 1.0);
            } else if ui.input_mut(|i| i.consume_key(egui::Modifiers::NONE, key)) {
                self.transform.pan += delta;
                self.transform.fit = false;
            }
        }
        if ui.input_mut(|i| {
            i.consume_key(egui::Modifiers::NONE, egui::Key::Plus)
                || i.consume_key(egui::Modifiers::NONE, egui::Key::Equals)
        }) {
            self.transform.zoom_at(1.25, Vec2::ZERO);
        }
        if ui.input_mut(|i| i.consume_key(egui::Modifiers::NONE, egui::Key::Minus)) {
            self.transform.zoom_at(1.0 / 1.25, Vec2::ZERO);
        }
    }
}
fn icon_button(ui: &mut egui::Ui, label: &str, selected: bool) -> egui::Response {
    let response = ui.add(egui::Button::new("").min_size(Vec2::splat(30.0)).selected(selected));
    response
        .widget_info(|| egui::WidgetInfo::selected(egui::accesskit::Role::Button, ui.is_enabled(), selected, label));
    if response.has_focus() {
        ui.painter().rect_stroke(
            response.rect.shrink(1.0),
            4.0,
            (2.0, Color32::WHITE),
            egui::StrokeKind::Inside,
        );
    }
    response.on_hover_text(label)
}
fn mode_button(ui: &mut egui::Ui, lang: Lang, mode: CompareMode, selected: bool, compact: bool) -> egui::Response {
    if !compact {
        return ui.selectable_label(selected, t(lang, mode.key()));
    }
    let response = icon_button(ui, t(lang, mode.key()), selected);
    let rect = response.rect.shrink(7.0);
    let p = ui.painter();
    let ink = ui.visuals().text_color();
    p.rect_stroke(rect, 1.0, (1.0, ink), egui::StrokeKind::Inside);
    match mode {
        CompareMode::Wipe => {
            p.rect_filled(
                Rect::from_min_max(rect.min, Pos2::new(rect.center().x, rect.bottom())),
                0.0,
                ink,
            );
        }
        CompareMode::SideBySide => {
            p.line_segment([rect.center_top(), rect.center_bottom()], (2.0, ink));
        }
        CompareMode::Flip => {
            p.rect_stroke(
                rect.translate(Vec2::new(3.0, -3.0)),
                1.0,
                (1.0, ink),
                egui::StrokeKind::Inside,
            );
        }
        CompareMode::Difference => {
            p.circle_filled(rect.center(), 4.0, Color32::from_rgb(240, 168, 69));
        }
    }
    response
}
fn paint_expand(ui: &egui::Ui, response: &egui::Response, close: bool) {
    let rect = response.rect.shrink(8.0);
    let p = ui.painter();
    let stroke = (1.5, ui.visuals().text_color());
    if close {
        p.line_segment([rect.left_top(), rect.right_bottom()], stroke);
        p.line_segment([rect.right_top(), rect.left_bottom()], stroke);
    } else {
        for (corner, offset) in [
            (rect.left_top(), Vec2::new(5.0, 5.0)),
            (rect.right_top(), Vec2::new(-5.0, 5.0)),
            (rect.left_bottom(), Vec2::new(5.0, -5.0)),
            (rect.right_bottom(), Vec2::new(-5.0, -5.0)),
        ] {
            p.line_segment([corner, corner + Vec2::new(offset.x, 0.0)], stroke);
            p.line_segment([corner, corner + Vec2::new(0.0, offset.y)], stroke);
        }
    }
}
fn image_label(painter: &egui::Painter, pos: Pos2, text: &str, right: bool) {
    let galley = painter.layout_no_wrap(text.into(), egui::FontId::proportional(14.0), Color32::WHITE);
    let pos = if right {
        pos - Vec2::new(galley.size().x, 0.0)
    } else {
        pos
    };
    painter.rect_filled(
        Rect::from_min_size(pos, galley.size()).expand2(Vec2::new(6.0, 4.0)),
        4.0,
        Color32::from_black_alpha(210),
    );
    painter.galley(pos, galley, Color32::WHITE);
}
fn provenance(ui: &mut egui::Ui, lang: Lang, pair: &Pair) {
    if pair.provenance.is_empty() {
        ui.label(t(lang, "no_provenance"));
        return;
    }
    let get = |key: &str| pair.provenance.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str());
    let settings = get("settings");
    let working = get("working_size")
        .or_else(|| settings.and_then(|s| s.split_whitespace().find_map(|v| v.strip_prefix("model="))));
    let key_width = pair
        .provenance
        .iter()
        .map(|(key, _)| key.as_str())
        .chain([
            t(lang, "model"),
            t(lang, "route"),
            t(lang, "working"),
            t(lang, "settings"),
        ])
        .map(|key| {
            ui.painter()
                .layout_no_wrap(
                    key.to_owned(),
                    egui::TextStyle::Body.resolve(ui.style()),
                    ui.visuals().text_color(),
                )
                .size()
                .x
                + 12.0
        })
        .fold(100.0_f32, f32::max);
    egui::Grid::new("compare-provenance")
        .min_col_width(key_width)
        .num_columns(2)
        .striped(true)
        .show(ui, |ui| {
            for (label, value) in [
                ("model", get("model_version")),
                ("route", get("route")),
                ("working", working),
                ("settings", settings),
            ] {
                ui.add(
                    egui::Label::new(egui::RichText::new(t(lang, label)).strong())
                        .wrap_mode(egui::TextWrapMode::Extend),
                );
                ui.add(egui::Label::new(value.unwrap_or(t(lang, "cmp.unknown"))).wrap());
                ui.end_row();
            }
            for (key, value) in &pair.provenance {
                if ["model_version", "route", "working_size", "settings"].contains(&key.as_str()) {
                    continue;
                }
                ui.add(egui::Label::new(key).wrap_mode(egui::TextWrapMode::Extend));
                ui.add(egui::Label::new(value).wrap());
                ui.end_row();
            }
        });
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cursor_zoom_preserves_the_image_coordinate() {
        let mut transform = Transform {
            scale: 0.5,
            pan: Vec2::new(30.0, -20.0),
            fit: false,
        };
        let cursor = Vec2::new(180.0, 100.0);
        let before = (cursor - transform.pan) / transform.scale;
        transform.zoom_at(2.0, cursor);
        assert!(((cursor - transform.pan) / transform.scale - before).length() < 1e-4);
    }
    #[test]
    fn new_view_starts_at_half_wipe() {
        let view = CompareView::new();
        assert_eq!(view.mode(), CompareMode::Wipe);
        assert_eq!(view.divider, 0.5);
    }
    #[test]
    fn default_view_also_starts_at_half_wipe() {
        assert_eq!(CompareView::default().divider, 0.5);
    }

    #[test]
    fn stale_target_and_selection_results_are_discarded() {
        let ctx = egui::Context::default();
        let (tx, _jobs) = mpsc::channel();
        let (results, rx) = mpsc::channel();
        let mut view = CompareView {
            epoch: 3,
            request: 8,
            loading: true,
            scanning: true,
            ..CompareView::default()
        };
        view.worker = Some(Worker {
            tx,
            rx,
            epoch: Arc::new(AtomicU64::new(3)),
            request: Arc::new(AtomicU64::new(8)),
        });
        results
            .send(Reply::Found(2, Ok((vec![], captures::CaptureHelp::default()))))
            .unwrap();
        results.send(Reply::Loaded(3, 7, Err(()))).unwrap();
        results.send(Reply::Loaded(2, 8, Err(()))).unwrap();
        view.receive(&ctx);
        assert!(view.scanning && view.loading);
        assert!(view.error.is_none());
        results.send(Reply::Loaded(3, 8, Err(()))).unwrap();
        view.receive(&ctx);
        assert!(!view.loading);
        assert_eq!(view.error, Some("load_error"));
    }

    #[test]
    fn real_panel_loads_switches_modes_flips_and_zooms() {
        use egui_kittest::{Harness, kittest::Queryable};
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("retoque-captures");
        std::fs::create_dir(&folder).unwrap();
        for (suffix, rgb) in [("original", [220, 40, 20, 255]), ("processed", [20, 40, 220, 255])] {
            captures::RgbaImage::from_pixel(8, 4, image::Rgba(rgb))
                .save(folder.join(format!("synthetic.{suffix}.png")))
                .unwrap();
        }
        let target = dir.path().to_owned();
        let mut harness = Harness::builder().with_size(Vec2::new(1100.0, 800.0)).build_ui_state(
            move |ui, view: &mut CompareView| view.ui(ui, Lang::En, &target),
            CompareView::default(),
        );
        let deadline = Instant::now() + Duration::from_secs(5);
        while harness.state().loaded.is_none() && Instant::now() < deadline {
            harness.run_steps(2);
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(harness.state().loaded.is_some());
        harness.run();
        assert_eq!(harness.state().selected, Some(0));
        let canvas = harness.get_by_label("Comparison image").rect();
        assert!(canvas.width() > 1000.0, "the image should use the whole panel width");
        let filmstrip = harness
            .get_by_role_and_label(egui::accesskit::Role::Button, "synthetic")
            .rect();
        assert!(filmstrip.top() > canvas.bottom(), "captures belong under the image");
        for (name, mode) in [
            ("Side by side", CompareMode::SideBySide),
            ("Difference", CompareMode::Difference),
            ("Flip", CompareMode::Flip),
        ] {
            harness.get_by_label(name).click();
            harness.run();
            assert_eq!(harness.state().mode, mode);
        }
        let texture_present = |h: &Harness<'_, CompareView>, id| {
            h.output()
                .shapes
                .iter()
                .any(|s| matches!(&s.shape,egui::Shape::Mesh(mesh) if mesh.texture_id == id))
        };
        let original = harness.state().loaded.as_ref().unwrap().original.id();
        let processed = harness.state().loaded.as_ref().unwrap().processed.id();
        assert!(texture_present(&harness, processed));
        assert!(!texture_present(&harness, original));
        // Clicking Flip leaves button focus. Space must still flip unless a text editor is focused.
        harness.key_down(egui::Key::Space);
        harness.run_steps(2);
        assert!(texture_present(&harness, original));
        assert!(!texture_present(&harness, processed));
        harness.key_up(egui::Key::Space);
        harness.run();
        assert!(texture_present(&harness, processed));
        harness.key_press(egui::Key::Num1);
        harness.run();
        assert_eq!(
            harness.state().mode,
            CompareMode::Flip,
            "viewer shortcuts require image focus"
        );
        harness.get_by_label("Wipe").click();
        harness.run();
        let canvas = harness.get_by_label("Comparison image").rect();
        harness.hover_at(canvas.center());
        harness.run_steps(1);
        harness.drag_at(canvas.center());
        harness.run_steps(1);
        harness.hover_at(canvas.center() - Vec2::new(30.0, 0.0));
        harness.run_steps(2);
        harness.drop_at(canvas.center() - Vec2::new(30.0, 0.0));
        harness.run();
        assert!(harness.state().divider < 0.5);
        harness.get_by_label("1:1").click();
        harness.run();
        assert!(!harness.state().transform.fit);
        harness.get_by_label("Fit").click();
        harness.run();
        assert!(harness.state().transform.fit);
        harness.hover_at(canvas.center() + Vec2::new(60.0, 10.0));
        harness.run_steps(1);
        let before = harness.state().transform.scale;
        harness.event(egui::Event::MouseWheel {
            unit: egui::MouseWheelUnit::Point,
            phase: egui::TouchPhase::Move,
            source: egui::MouseWheelSource::Wheel,
            delta: Vec2::new(0.0, 80.0),
            modifiers: egui::Modifiers::NONE,
        });
        harness.run();
        assert!(harness.state().transform.scale > before);
        harness.get_by_label("Comparison image").click();
        harness.run();
        for (key, mode) in [
            (egui::Key::Num2, CompareMode::SideBySide),
            (egui::Key::Num3, CompareMode::Flip),
            (egui::Key::Num4, CompareMode::Difference),
            (egui::Key::Num1, CompareMode::Wipe),
        ] {
            harness.key_press(key);
            harness.run();
            assert_eq!(harness.state().mode, mode);
        }
        harness.key_press(egui::Key::ArrowRight);
        harness.run();
        assert_eq!(harness.state().transform.pan.x, 24.0);
        let scale = harness.state().transform.scale;
        harness.key_press(egui::Key::Equals);
        harness.run();
        assert!(harness.state().transform.scale > scale);
        harness.key_press(egui::Key::F);
        harness.run();
        assert!(harness.state().transform.fit);
        assert_eq!(harness.state().transform.pan, Vec2::ZERO);
        harness.get_by_label("Enlarge comparison").click();
        harness.run();
        assert!(harness.state().expanded);
        harness.key_press(egui::Key::Escape);
        harness.run();
        assert!(!harness.state().expanded);
        assert!(harness.get_by_label("Enlarge comparison").is_focused());
    }
    #[test]
    fn hdr_panel_exposure_reset_and_all_background_thumbnails() {
        use egui_kittest::{Harness, kittest::Queryable};
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("retoque-captures");
        std::fs::create_dir(&folder).unwrap();
        for stem in ["older", "newer"] {
            for suffix in ["original", "processed"] {
                let mut bytes = b"PF\n8 4\n-1\n".to_vec();
                for _ in 0..8 * 4 * 3 {
                    bytes.extend_from_slice(&0.02_f32.to_le_bytes());
                }
                std::fs::write(folder.join(format!("{stem}.{suffix}.pfm")), bytes).unwrap();
            }
        }
        for stem in ["older", "newer"] {
            std::fs::write(
                folder.join(format!("{stem}.json")),
                r#"{"model_version":"synthetic","route":"test-route","settings":"model=8x4 scale=1"}"#,
            )
            .unwrap();
        }
        let target = dir.path().to_owned();
        let mut harness = Harness::builder().with_size(Vec2::new(566.0, 850.0)).build_ui_state(
            move |ui, view: &mut CompareView| view.ui(ui, Lang::Pt, &target),
            CompareView::default(),
        );
        fn settle(harness: &mut Harness<'_, CompareView>) {
            let deadline = Instant::now() + Duration::from_secs(5);
            while (harness.state().last_load_time().is_none() || harness.state().thumbnail_progress() != (2, 2))
                && Instant::now() < deadline
            {
                harness.run_steps(2);
                std::thread::sleep(Duration::from_millis(2));
            }
            assert!(harness.state().last_load_time().is_some());
            assert_eq!(harness.state().thumbnail_progress(), (2, 2));
            harness.run();
        }
        settle(&mut harness);
        for label in [
            "Divisor",
            "Lado a lado",
            "Alternar",
            "Diferença",
            "Ampliar comparação",
            "Redefinir exposição",
        ] {
            assert!(
                harness.get_by_label(label).rect().right() < 566.0,
                "{label} overflowed the compact toolbar"
            );
        }
        assert!(harness.query_by_label("Antes / depois").is_none());
        assert!(harness.query_by_label("Modelo").is_none());
        harness.get_by_label("Detalhes da captura").click();
        harness.run();
        assert!(harness.get_by_label("Modelo").rect().height() < 24.0);
        assert!(harness.get_by_label("Resolução de processamento").rect().height() < 24.0);
        let base = harness.state().loaded.as_ref().unwrap().exposure;
        harness.state_mut().set_exposure_ev(1.0);
        settle(&mut harness);
        assert!((harness.state().loaded.as_ref().unwrap().exposure / base - 2.0).abs() < 1e-5);
        harness.get_by_label("Redefinir exposição").click();
        settle(&mut harness);
        assert_eq!(harness.state().exposure_ev(), 0.0);
        assert_eq!(harness.state().loaded.as_ref().unwrap().exposure, base);
        // Reset must cancel a pending nonzero request even while the old 0-EV image remains visible.
        harness.state_mut().set_exposure_ev(4.0);
        harness.get_by_label("Redefinir exposição").click();
        settle(&mut harness);
        assert_eq!(harness.state().loaded.as_ref().unwrap().ev, 0.0);
    }
    #[test]
    fn empty_panel_explains_the_configured_shortcut() {
        use egui_kittest::{Harness, kittest::Queryable};
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("OptiScaler.ini"), "[Menu]\nShortcutKey=0x78\n").unwrap();
        let target = dir.path().to_owned();
        let mut harness = Harness::builder().with_size(Vec2::new(566.0, 700.0)).build_ui_state(
            move |ui, view: &mut CompareView| view.ui(ui, Lang::Pt, &target),
            CompareView::default(),
        );
        let deadline = Instant::now() + Duration::from_secs(5);
        while harness.state().scanning && Instant::now() < deadline {
            harness.run_steps(2);
            std::thread::sleep(Duration::from_millis(2));
        }
        harness.run();
        assert!(harness.get_by_label("Sua primeira comparação").rect().is_positive());
        let label = t(Lang::Pt, "cmp.empty.menu").replace("{key}", "F9");
        assert!(harness.get_by_label(&label).rect().right() <= 566.0);
        assert!(harness.get_by_label("Buscar capturas").rect().is_positive());
        assert!(harness.query_by_label("Comparison image").is_none());
    }
    #[test]
    fn thumbnail_cache_is_small_and_invalidates_on_source_change() {
        let dir = tempfile::tempdir().unwrap();
        let original = dir.path().join("thumb.original.png");
        captures::RgbaImage::from_pixel(8, 4, image::Rgba([220, 0, 0, 255]))
            .save(&original)
            .unwrap();
        let pair = Pair {
            stem: "thumb".into(),
            original: original.clone(),
            processed: original.clone(),
            sidecar: None,
            hdr: false,
            provenance: vec![],
        };
        let mut cache = ThumbnailCache::default();
        let first = cache.get(&pair).unwrap();
        let again = cache.get(&pair).unwrap();
        assert_eq!(first, again);
        assert_eq!(cache.images.len(), 1);
        assert_eq!(first.size, [8, 4]);
        captures::RgbaImage::from_pixel(12, 4, image::Rgba([0, 0, 220, 255]))
            .save(&original)
            .unwrap();
        let changed = cache.get(&pair).unwrap();
        assert_eq!(changed.size, [12, 4]);
        assert_ne!(first.pixels[0], changed.pixels[0]);
        assert_eq!(cache.images.len(), 1);
    }
}
