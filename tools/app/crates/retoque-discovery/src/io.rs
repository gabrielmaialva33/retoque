use std::fs::{self, File, Metadata, OpenOptions};
use std::io::Read;
use std::path::Path;

pub const MAX_TEXT_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_DB_BYTES: usize = 128 * 1024 * 1024;

/// Open a regular file without blocking on FIFOs or character devices, and validate the file
/// descriptor's metadata.
pub fn open_regular_file(path: &Path) -> Result<(File, Metadata), String> {
    let pre_meta = fs::metadata(path).map_err(|e| e.to_string())?;
    if !pre_meta.file_type().is_file() {
        return Err("not a regular file".to_string());
    }

    let mut opts = OpenOptions::new();
    opts.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        // O_NONBLOCK (0o4000 on Linux) ensures opening a FIFO swapped in after stat never blocks.
        opts.custom_flags(0o4000);
    }

    let file = opts.open(path).map_err(|e| e.to_string())?;
    let fd_meta = file.metadata().map_err(|e| e.to_string())?;
    if !fd_meta.file_type().is_file() {
        return Err("not a regular file".to_string());
    }

    Ok((file, fd_meta))
}

/// Read up to `max_bytes` from a validated regular file.
pub fn read_bounded_bytes(path: &Path, max_bytes: usize) -> Result<Vec<u8>, String> {
    let (file, meta) = open_regular_file(path)?;
    if meta.len() > max_bytes as u64 {
        return Err(format!(
            "file size ({} bytes) exceeds limit ({max_bytes} bytes)",
            meta.len()
        ));
    }

    let cap = (meta.len() as usize).min(max_bytes);
    let mut buf = Vec::with_capacity(cap);
    file.take((max_bytes + 1) as u64)
        .read_to_end(&mut buf)
        .map_err(|e| e.to_string())?;

    if buf.len() > max_bytes {
        return Err(format!("file exceeds limit ({max_bytes} bytes)"));
    }

    Ok(buf)
}

/// Read a UTF-8 string up to `max_bytes` from a validated regular file.
pub fn read_bounded_string(path: &Path, max_bytes: usize) -> Result<String, String> {
    let bytes = read_bounded_bytes(path, max_bytes)?;
    String::from_utf8(bytes).map_err(|e| format!("invalid UTF-8: {e}"))
}
