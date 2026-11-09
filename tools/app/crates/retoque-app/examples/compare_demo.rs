//! Read-only visual harness: compare_demo <game-folder|retoque-captures> [wipe|side|flip|diff] [EV].
//! 1–4 switches mode. No persistence, no game writes.
use retoque_app::{
    compare::{CompareMode, CompareView},
    i18n::{Lang, t},
};
use std::path::PathBuf;
struct Demo {
    view: CompareView,
    target: PathBuf,
    lang: Lang,
    reported: bool,
    thumbnails_reported: bool,
    initial_ev: Option<f32>,
}
impl eframe::App for Demo {
    fn persist_egui_memory(&self) -> bool {
        false
    }

    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading(t(self.lang, "title"));
            self.view.ui(ui, self.lang, &self.target);
        });
        if self.view.last_load_time().is_some()
            && let Some(ev) = self.initial_ev.take()
        {
            self.view.set_exposure_ev(ev);
        }
        let (loaded, total) = self.view.thumbnail_progress();
        if !self.thumbnails_reported && total > 0 && loaded == total {
            eprintln!("COMPARE_THUMBNAILS_READY count={total}");
            self.thumbnails_reported = true;
        }
        if !self.reported
            && let Some(elapsed) = self.view.last_load_time()
        {
            eprintln!("COMPARE_READY cpu_load_seconds={:.3}", elapsed.as_secs_f64());
            self.reported = true;
        }
    }
}
fn main() -> eframe::Result {
    let mut args = std::env::args_os().skip(1);
    let Some(target) = args.next() else {
        eprintln!("compare_demo <game-folder|retoque-captures> [wipe|side|flip|diff] [EV]");
        return Ok(());
    };
    let mode = match args.next().and_then(|arg| arg.into_string().ok()).as_deref() {
        Some("side") => CompareMode::SideBySide,
        Some("flip") => CompareMode::Flip,
        Some("diff") => CompareMode::Difference,
        _ => CompareMode::Wipe,
    };
    let initial_ev = args
        .next()
        .and_then(|arg| arg.into_string().ok())
        .and_then(|arg| arg.parse::<f32>().ok());
    let lang = Lang::from_system();
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1440.0, 1000.0])
            .with_app_id("retoque-compare-demo"),
        persist_window: false,
        // Explicit null storage avoids eframe creating a data directory just to read its default
        // state file. With window/egui/app persistence disabled, this storage is never dirtied.
        persistence_path: Some(PathBuf::from(if cfg!(windows) { "NUL" } else { "/dev/null" })),
        ..Default::default()
    };
    eframe::run_native(
        t(lang, "window"),
        options,
        Box::new(move |cc| {
            retoque_app::theme::apply(&cc.egui_ctx);
            let mut view = CompareView::new();
            view.set_mode(mode);
            Ok(Box::new(Demo {
                view,
                target: PathBuf::from(target),
                lang,
                reported: false,
                thumbnails_reported: false,
                initial_ev,
            }))
        }),
    )
}
