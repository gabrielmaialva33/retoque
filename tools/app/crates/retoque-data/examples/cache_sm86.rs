//! Explicit kit-build opt-in. This program contacts the official release and writes only its destination.
fn main() -> std::io::Result<()> {
    let mut args = std::env::args_os().skip(1);
    let consent = args.next();
    let dest = args.next();
    if consent.as_deref() != Some(std::ffi::OsStr::new("--download-official-release"))
        || dest.is_none()
        || args.next().is_some()
    {
        return Err(std::io::Error::other(
            "usage: cache_sm86 --download-official-release DESTINATION",
        ));
    }
    retoque_data::sm86::build_first_run_cache(std::path::Path::new(&dest.unwrap()), &retoque_data::sm86::GithubFetcher)
}
