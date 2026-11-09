//! Offline verification/import of a first-run cache into an explicit scratch destination.
fn main() -> std::io::Result<()> {
    let mut args = std::env::args_os().skip(1);
    let source = args
        .next()
        .ok_or_else(|| std::io::Error::other("source cache required"))?;
    let destination = args
        .next()
        .ok_or_else(|| std::io::Error::other("scratch destination required"))?;
    if args.next().is_some() {
        return Err(std::io::Error::other("unexpected argument"));
    }
    let root = std::path::Path::new(&destination);
    let cache = retoque_data::sm86::Cache::open(&root.join("releases"))?;
    let catalog = retoque_data::components::ComponentCatalog::open(Some(&root.join("components")))?;
    let (component, record) =
        retoque_data::sm86::import_first_run_cache(std::path::Path::new(&source), &cache, &catalog)?;
    println!(
        "version={} files={} ignored={} sha256={}",
        record.release.version,
        component.files.len(),
        record.ignored.len(),
        record.sha256
    );
    Ok(())
}
