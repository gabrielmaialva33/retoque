//! Bounded, platform-independent VERSIONINFO reader. Follows the PE resource tree; never scans for signatures.
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

const VS_VERSION_INFO_UTF16LE: [u8; 32] = [
    b'V', 0, b'S', 0, b'_', 0, b'V', 0, b'E', 0, b'R', 0, b'S', 0, b'I', 0, b'O', 0, b'N', 0, b'_', 0, b'I', 0, b'N',
    0, b'F', 0, b'O', 0, 0, 0,
];

fn u16_at(b: &[u8], n: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(n..n.checked_add(2)?)?.try_into().ok()?))
}

fn u32_at(b: &[u8], n: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(n..n.checked_add(4)?)?.try_into().ok()?))
}

fn read_exact_at<R: Read + Seek>(r: &mut R, at: u64, out: &mut [u8]) -> Option<()> {
    r.seek(SeekFrom::Start(at)).ok()?;
    r.read_exact(out).ok()
}

fn read_at<R: Read + Seek>(r: &mut R, at: u64, len: usize) -> Option<Vec<u8>> {
    if len > 4 * 1024 * 1024 {
        return None;
    }
    r.seek(SeekFrom::Start(at)).ok()?;
    if len <= 65536 {
        let mut b = vec![0; len];
        r.read_exact(&mut b).ok()?;
        Some(b)
    } else {
        let mut b = Vec::with_capacity(65536);
        r.take(len as u64).read_to_end(&mut b).ok()?;
        (b.len() == len).then_some(b)
    }
}

pub fn file_version(path: &Path) -> Option<String> {
    let mut file = File::open(path).ok()?;
    if !file.metadata().ok()?.is_file() {
        return None;
    }
    read_version(&mut file)
}

fn entries(b: &[u8], at: usize) -> Option<impl Iterator<Item = (u32, u32)> + '_> {
    let count = usize::from(u16_at(b, at.checked_add(12)?)?) + usize::from(u16_at(b, at.checked_add(14)?)?);
    if count > 128 {
        return None;
    }
    let start = at.checked_add(16)?;
    let end = start.checked_add(count * 8)?;
    let table = b.get(start..end)?;
    Some(table.as_chunks::<8>().0.iter().map(|c| {
        (
            u32::from_le_bytes([c[0], c[1], c[2], c[3]]),
            u32::from_le_bytes([c[4], c[5], c[6], c[7]]),
        )
    }))
}

fn fixed_pair(b: &[u8]) -> Option<(u32, u32)> {
    let len = usize::from(u16_at(b, 0)?);
    let b = b.get(..len)?;
    if u16_at(b, 2)? != 52 || u16_at(b, 4)? != 0 {
        return None;
    }
    if b.get(6..6 + VS_VERSION_INFO_UTF16LE.len())? != VS_VERSION_INFO_UTF16LE {
        return None;
    }
    let at = (6 + VS_VERSION_INFO_UTF16LE.len() + 3) & !3;
    b.get(at..at + 52)?;
    if u32_at(b, at)? != 0xfeef04bd || u32_at(b, at + 4)? != 0x10000 {
        return None;
    }
    let ms = u32_at(b, at + 8)?;
    let ls = u32_at(b, at + 12)?;
    Some((ms, ls))
}

/// Limits: 96 sections, 128 entries/directory, three resource levels, 4 MiB resource region.
pub fn read_version<R: Read + Seek>(r: &mut R) -> Option<String> {
    let mut dos = [0u8; 64];
    read_exact_at(r, 0, &mut dos)?;
    if &dos[..2] != b"MZ" {
        return None;
    }
    let pe = u64::from(u32_at(&dos, 60)?);
    if pe > 16 * 1024 * 1024 {
        return None;
    }
    let mut coff = [0u8; 24];
    read_exact_at(r, pe, &mut coff)?;
    if &coff[..4] != b"PE\0\0" {
        return None;
    }
    let sections = usize::from(u16_at(&coff, 6)?);
    if sections == 0 || sections > 96 {
        return None;
    }
    let optional_len = usize::from(u16_at(&coff, 20)?);
    if optional_len > 4096 {
        return None;
    }
    let mut optional_buf = [0u8; 4096];
    let optional = &mut optional_buf[..optional_len];
    read_exact_at(r, pe + 24, optional)?;
    let dir = match u16_at(optional, 0)? {
        0x10b => 96,
        0x20b => 112,
        _ => return None,
    };
    if u32_at(optional, dir - 4)? < 3 {
        return None;
    }
    let rva = u32_at(optional, dir + 16)?;
    let size = u32_at(optional, dir + 20)?;
    if rva == 0 || !(16..=4 * 1024 * 1024).contains(&size) {
        return None;
    }
    let mut table_buf = [0u8; 96 * 40];
    let table = &mut table_buf[..sections * 40];
    read_exact_at(r, pe + 24 + optional_len as u64, table)?;
    let mut resource = None;
    for s in table.as_chunks::<40>().0 {
        let va = u32_at(s, 12)?;
        let raw_size = u32_at(s, 16)?;
        let raw = u32_at(s, 20)?;
        if let Some(delta) = rva.checked_sub(va)
            && delta.checked_add(size).is_some_and(|end| end <= raw_size)
        {
            if resource.is_some() {
                return None;
            } // overlapping mappings are ambiguous
            resource = Some(read_at(r, u64::from(raw) + u64::from(delta), size as usize)?);
        }
    }
    let resource = resource?;
    let mut first_ver: Option<(u32, u32)> = None;
    let mut leaves = 0usize;
    let mut dirs = 0usize;
    for (kind, names) in entries(&resource, 0)? {
        if kind != 16 {
            continue;
        }
        if names & 0x80000000 == 0 {
            return None;
        }
        dirs += 1;
        if dirs > 128 {
            return None;
        }
        for (_, langs) in entries(&resource, (names & 0x7fffffff) as usize)? {
            if langs & 0x80000000 == 0 {
                return None;
            }
            dirs += 1;
            if dirs > 128 {
                return None;
            }
            for (_, leaf) in entries(&resource, (langs & 0x7fffffff) as usize)? {
                leaves += 1;
                if leaves > 128 || leaf & 0x80000000 != 0 {
                    return None;
                }
                let leaf = leaf as usize;
                let data = u32_at(&resource, leaf)?.checked_sub(rva)? as usize;
                let len = u32_at(&resource, leaf.checked_add(4)?)? as usize;
                let ver = fixed_pair(resource.get(data..data.checked_add(len)?)?)?;
                match first_ver {
                    None => first_ver = Some(ver),
                    Some(prev) if prev != ver => return None,
                    Some(_) => {}
                }
            }
        }
    }
    let (ms, ls) = first_ver?;
    Some(format!("{}.{}.{}.{}", ms >> 16, ms & 65535, ls >> 16, ls & 65535))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    fn put16(b: &mut [u8], n: usize, v: u16) {
        b[n..n + 2].copy_from_slice(&v.to_le_bytes());
    }
    fn put32(b: &mut [u8], n: usize, v: u32) {
        b[n..n + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn fixture(pe64: bool) -> Vec<u8> {
        let mut b = vec![0; 1024];
        b[..2].copy_from_slice(b"MZ");
        put32(&mut b, 60, 64);
        b[64..68].copy_from_slice(b"PE\0\0");
        put16(&mut b, 70, 1);
        let ol = if pe64 { 240 } else { 224 };
        put16(&mut b, 84, ol);
        put16(&mut b, 88, if pe64 { 0x20b } else { 0x10b });
        let d = 88 + if pe64 { 112 } else { 96 };
        put32(&mut b, d - 4, 16);
        put32(&mut b, d + 16, 0x1000);
        put32(&mut b, d + 20, 256);
        let s = 88 + ol as usize;
        put32(&mut b, s + 12, 0x1000);
        put32(&mut b, s + 16, 256);
        put32(&mut b, s + 20, 512);
        let r = &mut b[512..];
        for (at, id, to) in [(0, 16, 0x80000020), (32, 1, 0x80000040), (64, 1033, 96)] {
            put16(r, at + 14, 1);
            put32(r, at + 16, id);
            put32(r, at + 20, to);
        }
        put32(r, 96, 0x1080);
        put32(r, 100, 92);
        put16(r, 128, 92);
        put16(r, 130, 52);
        for (i, c) in "VS_VERSION_INFO\0".encode_utf16().enumerate() {
            put16(r, 134 + i * 2, c);
        }
        put32(r, 168, 0xfeef04bd);
        put32(r, 172, 0x10000);
        put32(r, 176, 1 << 16 | 2);
        put32(r, 180, 3 << 16 | 4);
        b
    }
    #[test]
    fn pe32_and_pe64_resource_versions() {
        for pe64 in [false, true] {
            assert_eq!(
                read_version(&mut Cursor::new(fixture(pe64))).as_deref(),
                Some("1.2.3.4")
            );
        }
    }
    #[test]
    fn game_status_reads_versions_and_only_warns_on_known_mismatch() {
        let temp = tempfile::tempdir().unwrap();
        let mut proxy = fixture(true);
        proxy.extend_from_slice(b"OptiScaler working as");
        std::fs::write(temp.path().join("dxgi.dll"), &proxy).unwrap();
        std::fs::write(temp.path().join(crate::FORWARDER_NAME), fixture(false)).unwrap();
        let status = crate::status(temp.path()).unwrap();
        assert_eq!(status.proxy_version.as_deref(), Some("1.2.3.4"));
        assert_eq!(status.forwarder_version, status.proxy_version);
        assert!(!status.version_mismatch());
        let mut other = fixture(false);
        put32(&mut other, 512 + 180, 3 << 16 | 5);
        std::fs::write(temp.path().join(crate::FORWARDER_NAME), other).unwrap();
        assert!(crate::status(temp.path()).unwrap().version_mismatch());
        std::fs::write(temp.path().join(crate::FORWARDER_NAME), b"unversioned forwarder").unwrap();
        let status = crate::status(temp.path()).unwrap();
        assert!(status.forwarder_version.is_none());
        assert!(!status.version_mismatch());
        assert_eq!(std::fs::read(temp.path().join("dxgi.dll")).unwrap(), proxy);
    }

    #[test]
    fn missing_truncated_forged_and_cyclic_resources_are_unknown() {
        let b = fixture(true);
        for n in 0..732 {
            assert_eq!(read_version(&mut Cursor::new(&b[..n])), None);
        }
        for (offset, value) in [
            (112 + 88 + 16, 0),
            (512 + 20, 0x80000000),
            (512 + 96, 0xffffffff),
            (512 + 100, 0xffffffff),
            (512 + 168, 0),
        ] {
            let mut bad = b.clone();
            put32(&mut bad, offset, value);
            assert_eq!(read_version(&mut Cursor::new(bad)), None);
        }
        assert_eq!(read_version(&mut Cursor::new(&b[640..])), None);
    }
}
