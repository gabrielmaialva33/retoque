//! Shared synchronous HTTPS transport; each caller supplies its own source/redirect policy.
use std::{
    io::{self, Read},
    time::Duration,
};
fn invalid(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, s)
}
pub(crate) fn get(
    url: &str,
    limit: u64,
    progress: &mut dyn FnMut(u64),
    allowed: impl Fn(&str, bool) -> bool,
) -> io::Result<Vec<u8>> {
    if !allowed(url, false) {
        return Err(invalid("unapproved download URL"));
    }
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .https_only(true)
        .max_redirects(0)
        .proxy(None)
        .timeout_global(Some(Duration::from_secs(180)))
        .user_agent("Retoque-sm86-release")
        .build()
        .into();
    let mut current = url.to_owned();
    for _ in 0..5 {
        if !allowed(&current, current != url) {
            return Err(invalid("refused sm86 release redirect"));
        }
        let mut response = agent
            .get(&current)
            .call()
            .map_err(|_| invalid("sm86 HTTPS request failed"))?;
        if response.status().is_redirection() {
            let location = response
                .headers()
                .get("location")
                .and_then(|s| s.to_str().ok())
                .ok_or_else(|| invalid("missing release redirect"))?;
            // GitHub release redirects are absolute HTTPS URLs. Refuse relative URLs.
            if !allowed(location, true) {
                return Err(invalid("refused release redirect"));
            }
            current = location.to_owned();
            continue;
        }
        if response.status().as_u16() != 200 {
            return Err(invalid("unexpected release HTTP status"));
        }
        if response
            .headers()
            .get("content-length")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.parse::<u64>().ok())
            .is_some_and(|n| n > limit)
        {
            return Err(invalid("release response exceeds size limit"));
        }
        let mut out = Vec::new();
        let mut reader = response.body_mut().as_reader();
        let mut buf = [0u8; 65536];
        loop {
            let n = reader.read(&mut buf)?;
            if n == 0 {
                break;
            }
            if out.len() as u64 + n as u64 > limit {
                return Err(invalid("release response exceeds size limit"));
            }
            out.extend_from_slice(&buf[..n]);
            progress(out.len() as u64);
        }
        return Ok(out);
    }
    Err(invalid("too many release redirects"))
}
