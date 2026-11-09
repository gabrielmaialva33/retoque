//! Offline parity/diagnostic runner. Requires local compiler libraries or a pinned archive; never downloads.
use retoque_data::lsfg_prepare::{self as prepare, Progress};
use std::{
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
    time::Instant,
};
struct Archive<'a>(&'a Path);
impl prepare::download::Fetcher for Archive<'_> {
    fn fetch(&self, _url: &str, out: &mut dyn Write, _cancel: &AtomicBool) -> prepare::Result<()> {
        std::io::copy(
            &mut std::fs::File::open(self.0)?.take(prepare::download::MAX_DOWNLOAD + 1),
            out,
        )?;
        Ok(())
    }
}
fn main() {
    let mut args: Vec<PathBuf> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    let archive = args.first().is_some_and(|s| s == Path::new("--archive"));
    if archive {
        args.remove(0);
    }
    assert_eq!(
        args.len(),
        3,
        "usage: prepare_lsfg [--archive] <Lossless Scaling folder> <app data> <compiler folder or pinned archive>"
    );
    let start = Instant::now();
    let cancel = AtomicBool::new(false);
    let mut progress = |p| {
        if let Progress::Compiling { done, total } = p
            && done % 10 == 0
        {
            eprintln!("{done}/{total}");
        }
    };
    let result = if archive {
        prepare::ensure_with_fetcher(
            &args[0],
            &args[1],
            &args[1].join("no-bundled-dxc"),
            true,
            &Archive(&args[2]),
            &cancel,
            &mut progress,
        )
    } else {
        prepare::ensure_lsfg_shaders(&args[0], &args[1], &args[2], false, &cancel, &mut progress)
    }
    .expect("shader preparation");
    println!(
        "99 shaders ready in {:.3}s; DLL SHA-256 {}; cache {}",
        start.elapsed().as_secs_f64(),
        result.dll_sha256,
        result.directory.display()
    );
}
