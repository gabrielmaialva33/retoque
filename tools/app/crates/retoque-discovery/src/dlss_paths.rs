//! Complete, bounded paths for updates, additive to the legacy presence-only discovery API.
use std::{
    fs, io,
    path::{Path, PathBuf},
};
/// Relative paths below `install_dir`; no links/reparse points or known mod/catalog trees.
/// Incomplete scans fail closed: callers must not update just the first copies they found.
pub fn find_dlss_paths(install_dir: &Path) -> io::Result<Vec<String>> {
    fn walk(root: &Path, rel: &Path, depth: usize, budget: &mut usize, out: &mut Vec<String>) -> io::Result<()> {
        if depth > 24 {
            return Err(io::Error::other("DLSS scan depth exceeded"));
        }
        for entry in fs::read_dir(root.join(rel))? {
            let entry = entry?;
            if *budget == 0 {
                return Err(io::Error::other("DLSS scan entry limit exceeded"));
            }
            *budget -= 1;
            let meta = fs::symlink_metadata(entry.path())?;
            #[cfg(windows)]
            {
                use std::os::windows::fs::MetadataExt;
                if meta.file_attributes() & 0x400 != 0 {
                    continue;
                }
            }
            if meta.file_type().is_symlink() {
                continue;
            }
            let name = entry.file_name();
            let Some(name) = name.to_str() else {
                return Err(io::Error::other("non-Unicode DLSS scan path"));
            };
            let low = name.to_ascii_lowercase();
            let next = rel.join(name);
            if meta.is_dir() {
                if ![".retoque", "dlssg_sm86", "optiscaler", "dlssnrmodels", ".git"].contains(&low.as_str()) {
                    walk(root, &next, depth + 1, budget, out)?;
                }
            } else if meta.is_file() && ["nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"].contains(&low.as_str())
            {
                // Reject literal backslashes on Unix instead of interpreting them as separators.
                if next
                    .components()
                    .any(|c| c.as_os_str().to_string_lossy().contains('\\'))
                {
                    return Err(io::Error::other("invalid DLSS path"));
                }
                out.push(next.to_string_lossy().replace('\\', "/"));
            }
        }
        Ok(())
    }
    let root: PathBuf = install_dir.canonicalize()?;
    let mut out = Vec::new();
    walk(&root, Path::new(""), 0, &mut 100_000, &mut out)?;
    out.sort();
    Ok(out)
}
