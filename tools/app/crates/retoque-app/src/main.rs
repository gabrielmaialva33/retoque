//! Retoque's desktop app: install, update and remove Retoque per game, change its settings, choose the NR model and
//! read the logs, the same on Linux and Windows (docs/superpowers/specs/2026-11-05-retoque-app.md).

#![cfg_attr(windows, windows_subsystem = "windows")]

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Retoque")
            .with_app_id("retoque")
            .with_inner_size([1180.0, 760.0])
            .with_min_inner_size([820.0, 520.0]),
        #[cfg(windows)]
        renderer: eframe::Renderer::Wgpu,
        #[cfg(not(windows))]
        renderer: eframe::Renderer::Glow,
        ..Default::default()
    };
    eframe::run_native(
        "Retoque",
        options,
        Box::new(|cc| {
            egui_extras::install_image_loaders(&cc.egui_ctx);
            Ok(Box::new(retoque_app::RetoqueApp::new(cc)))
        }),
    )
}
