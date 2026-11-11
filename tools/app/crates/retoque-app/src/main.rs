//! Retoque's desktop app: install, update and remove Retoque per game, change its settings, choose the NR model and
//! read the logs, the same on Linux and Windows (docs/superpowers/specs/2026-11-05-retoque-app.md).

#![cfg_attr(windows, windows_subsystem = "windows")]

fn main() -> eframe::Result {
    if let Ok(dir) = retoque_data::lsfg_prepare::data_dir() {
        retoque_data::model_facts::persist_in(&dir);
        retoque_core::memo::persist_in(&dir);
    }
    if let Ok(dir) = retoque_data::lsfg_prepare::data_dir()
        && retoque_app::applog::init(&dir).is_some()
    {
        let default_hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            retoque_app::applog::line(format_args!("panic: {info}"));
            default_hook(info);
        }));
    }
    let mut viewport = egui::ViewportBuilder::default()
        .with_title("Retoque")
        .with_app_id("retoque")
        .with_inner_size([1180.0, 760.0])
        .with_min_inner_size([820.0, 520.0]);
    if let Some(icon) = window_icon() {
        viewport = viewport.with_icon(icon);
    }
    let options = eframe::NativeOptions {
        viewport,
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

/// The window and taskbar icon (assets/retoque.svg rendered at 256 px). The Windows executable also carries
/// assets/retoque.ico through build.rs, for Explorer and shortcuts.
fn window_icon() -> Option<egui::IconData> {
    let image =
        image::load_from_memory_with_format(include_bytes!("../assets/retoque-256.png"), image::ImageFormat::Png)
            .ok()?
            .into_rgba8();
    let (width, height) = image.dimensions();
    Some(egui::IconData {
        rgba: image.into_raw(),
        width,
        height,
    })
}

#[cfg(test)]
mod tests {
    #[test]
    fn the_window_icon_decodes() {
        let icon = super::window_icon().expect("assets/retoque-256.png decodes");
        assert_eq!((icon.width, icon.height), (256, 256));
        assert_eq!(icon.rgba.len(), 256 * 256 * 4);
    }
}
