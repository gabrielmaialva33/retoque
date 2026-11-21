//! Check a built `Retoque-Setup-<version>.exe` on any platform without running it: read its bundle as the stub does,
//! verify the release metadata, `SHA256SUMS` and archive together, and unpack the archive with the app's own
//! validator into a temporary folder, then stage it as the app's `--setup` update does (`app_release::stage`, which
//! also checks the executable's version marker). Both temporary folders are removed afterwards.
//!
//! ```sh
//! cargo run -p retoque-setup --example inspect_setup -- release/app/Retoque-Setup-0.3.2.exe
//! ```
use std::{fs, io, path::Path};

fn count(dir: &Path) -> io::Result<(usize, u64)> {
    let mut total = (0, 0);
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let meta = entry.metadata()?;
        if meta.is_dir() {
            let (n, bytes) = count(&entry.path())?;
            total = (total.0 + n, total.1 + bytes);
        } else {
            total = (total.0 + 1, total.1 + meta.len());
        }
    }
    Ok(total)
}

fn main() -> io::Result<()> {
    let path = std::env::args_os()
        .nth(1)
        .ok_or_else(|| io::Error::other("usage: inspect_setup <Retoque-Setup-version.exe>"))?;
    let file_len = fs::metadata(&path)?.len();
    let bundle = retoque_setup::read_bundle(&mut fs::File::open(&path)?)?;
    let release = bundle.release()?;
    let stub = file_len
        - (bundle.zip.len() + bundle.sums.len() + bundle.release_json.len() + retoque_setup::TRAILER_LEN) as u64;
    println!("setup: {file_len} bytes, stub {stub} bytes");
    println!(
        "release: {} ({}), published {}\napp archive: {} ({} bytes, sha256 {})\nSHA256SUMS: {} bytes, release.json: {} bytes",
        release.tag,
        release.version,
        release.published_at,
        release.asset,
        release.size,
        release.digest,
        bundle.sums.len(),
        bundle.release_json.len()
    );
    let root = tempfile::Builder::new().prefix("retoque-setup-inspect-").tempdir()?;
    let dir = root.path().join("setup");
    fs::create_dir(&dir)?;
    let app = bundle.extract(&dir)?;
    let (files, bytes) = count(&dir.join("payload"))?;
    println!(
        "unpacked with the app's validator: {files} files, {bytes} bytes; app {}",
        app.strip_prefix(&dir).unwrap_or(&app).display()
    );
    let install = root.path().join("install");
    fs::create_dir(&install)?;
    let staged = retoque_data::app_release::stage(&release, &bundle.zip, &bundle.sums, &install)?;
    println!(
        "staged as an update: {} files in the manifest, version {}",
        staged.manifest.files.len(),
        staged.manifest.release.version
    );
    Ok(())
}
