//! Static CUDA target evidence, never runtime qualification. No model bytes are bundled.
use crate::gpu::{Gpu, Vendor};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    io::{self, Read, Seek, SeekFrom},
    sync::{Mutex, OnceLock},
};

pub const MAX_MODEL_BYTES: u64 = 512 * 1024 * 1024;
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ModelCompatibility {
    pub archs: Vec<u16>,
    /// Exact content hash + length from the locally inspected corpus.
    pub known: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Compatibility {
    Compatible,
    Incompatible,
    Unknown,
}
impl ModelCompatibility {
    pub fn for_gpu(&self, gpu: Option<&Gpu>) -> Compatibility {
        let Some(gpu) = gpu else { return Compatibility::Unknown };
        if gpu.vendor != Vendor::Nvidia {
            return Compatibility::Incompatible;
        }
        let Some((major, minor)) = gpu.compute_cap else {
            return Compatibility::Unknown;
        };
        if major > 99 || minor > 9 {
            return Compatibility::Unknown;
        }
        if !self.known || self.archs.is_empty() {
            return Compatibility::Unknown;
        }
        // Conservative for newer architecture-specific/family-specific targets: exact only.
        if self.archs.iter().any(|&sm| {
            u32::from(sm) == major * 10 + minor
                || (major < 10 && u32::from(sm / 10) == major && u32::from(sm % 10) <= minor)
        }) {
            Compatibility::Compatible
        } else if major < 12 {
            Compatibility::Incompatible
        } else {
            Compatibility::Unknown
        }
    }
}

// SHA256, byte count, CUDA SASS targets. Derived from validated fatbin v1 headers (including compressed
// payloads), with ELF ABI 7/8 corroboration for the official builds. PTX in this corpus targets compute_120.
// A matching target is a prerequisite, not a claim that every kernel or game route has been tested.
const KNOWN: &[(&str, u64, &[u16])] = &[
    (
        "4b8d19bc3eff58a084f5eca7489c921501c203450169fb82ff4f649a4482ba05",
        165840496,
        &[89, 120],
    ),
    (
        "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a",
        165840496,
        &[75, 86, 89, 120],
    ),
    (
        "89fa32d64952fef1845ba6de7f0141d3f7c809929ff17890cf7f27e049a35a10",
        165830144,
        &[89, 120],
    ),
    (
        "7e042cdfefe611e1aea2049e381fd5f757e15ff3953b3fecea72ab09f0d4a603",
        165830144,
        &[86, 120],
    ),
    (
        "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
        165840496,
        &[120],
    ),
    (
        "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927",
        165830144,
        &[75, 86, 89, 120],
    ),
    (
        "f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc",
        165840496,
        &[75, 86, 89, 120],
    ),
    (
        "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed",
        276162560,
        &[75, 86, 89, 120],
    ),
    (
        "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f",
        266424944,
        &[120],
    ),
];
fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "model inspection limit or malformed input")
}
fn u16_at(b: &[u8], n: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(n..n + 2)?.try_into().ok()?))
}
fn u32_at(b: &[u8], n: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(n..n + 4)?.try_into().ok()?))
}
fn u64_at(b: &[u8], n: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(n..n + 8)?.try_into().ok()?))
}
fn elf_arch(h: &[u8]) -> Option<u16> {
    if h.get(..4)? != b"\x7fELF" || h.get(5..7)? != [1, 1] || u16_at(h, 18)? != 190 || u32_at(h, 20)? != 1 {
        return None;
    }
    let (flags_at, size_at, size) = match h[4] {
        1 => (36, 40, 52),
        2 => (48, 52, 64),
        _ => return None,
    };
    h.get(..usize::from(size))?;
    if u16_at(h, size_at)? != size {
        return None;
    }
    let flags = u32_at(h, flags_at)?;
    let sm = match (h[7], h[8]) {
        (0x33, 0..=7) => flags & 255,
        (0x41, 8) => (flags >> 8) & 255,
        _ => return None,
    };
    (20..=130).contains(&sm).then_some(sm as u16)
}
fn fatbin<R: Read + Seek>(r: &mut R, offset: u64, len: u64, budget: &mut usize) -> io::Result<Vec<u16>> {
    r.seek(SeekFrom::Start(offset))?;
    let mut h = [0; 16];
    r.read_exact(&mut h)?;
    if h[..8] != [0x50, 0xed, 0x55, 0xba, 1, 0, 16, 0] {
        return Ok(vec![]);
    }
    let Some(end) = (offset + 16)
        .checked_add(u64_at(&h, 8).unwrap_or(u64::MAX))
        .filter(|&v| v <= len)
    else {
        return Ok(vec![]);
    };
    let mut pos = offset + 16;
    let mut archs = vec![];
    for _ in 0..4096 {
        if pos == end {
            return Ok(archs);
        }
        if end - pos < 64 {
            return Ok(vec![]);
        }
        if *budget == 0 {
            return Err(invalid());
        }
        *budget -= 1;
        r.seek(SeekFrom::Start(pos))?;
        let mut e = [0; 64];
        r.read_exact(&mut e)?;
        let kind = u16_at(&e, 0).unwrap_or(0);
        let header = u64::from(u32_at(&e, 4).unwrap_or(0));
        let size = u64_at(&e, 8).unwrap_or(u64::MAX);
        let sm = u32_at(&e, 28).unwrap_or(0);
        let next = pos.checked_add(header).and_then(|p| p.checked_add(size));
        if !matches!(kind, 1 | 2)
            || u16_at(&e, 2) != Some(0x101)
            || !(64..=4096).contains(&header)
            || !(20..=130).contains(&sm)
            || size == 0
            || next.is_none_or(|n| n > end)
        {
            return Ok(vec![]);
        }
        if kind == 2 {
            archs.push(sm as u16);
        }
        pos = next.unwrap_or(end);
    }
    Ok(vec![])
}

const LO_BITS: u64 = 0x0101_0101_0101_0101;
const HI_BITS: u64 = 0x8080_8080_8080_8080;

#[inline(always)]
fn has_byte(word: u64, byte: u8) -> u64 {
    let x = word ^ (LO_BITS * u64::from(byte));
    x.wrapping_sub(LO_BITS) & !x & HI_BITS
}

#[inline(always)]
fn check_position(
    window: &[u8],
    i: usize,
    base: u64,
    archs: &mut BTreeSet<u16>,
    fatbin_offsets: &mut Vec<u64>,
    candidates: &mut usize,
) -> io::Result<()> {
    let b0 = window[i];
    if b0 == 0x7f {
        if window.get(i + 1..i + 4) == Some(b"ELF".as_slice())
            && let Some(sm) = elf_arch(&window[i..])
        {
            *candidates += 1;
            if *candidates > 4096 {
                return Err(invalid());
            }
            archs.insert(sm);
        }
    } else if b0 == 0x50 && window.get(i + 1..i + 4) == Some([0xed, 0x55, 0xba].as_slice()) {
        fatbin_offsets.push(base + i as u64);
        if *candidates + fatbin_offsets.len() > 4096 {
            return Err(invalid());
        }
    }
    Ok(())
}

fn scan_window(
    window: &[u8],
    scan_end: usize,
    base: u64,
    archs: &mut BTreeSet<u16>,
    fatbin_offsets: &mut Vec<u64>,
    candidates: &mut usize,
) -> io::Result<()> {
    let mut i = 0;
    while i + 32 <= scan_end {
        let w0 = u64::from_le_bytes(window[i..i + 8].try_into().unwrap());
        let w1 = u64::from_le_bytes(window[i + 8..i + 16].try_into().unwrap());
        let w2 = u64::from_le_bytes(window[i + 16..i + 24].try_into().unwrap());
        let w3 = u64::from_le_bytes(window[i + 24..i + 32].try_into().unwrap());
        let m0 = has_byte(w0, 0x7f) | has_byte(w0, 0x50);
        let m1 = has_byte(w1, 0x7f) | has_byte(w1, 0x50);
        let m2 = has_byte(w2, 0x7f) | has_byte(w2, 0x50);
        let m3 = has_byte(w3, 0x7f) | has_byte(w3, 0x50);
        if (m0 | m1 | m2 | m3) != 0 {
            for (off, mut mask) in [(i, m0), (i + 8, m1), (i + 16, m2), (i + 24, m3)] {
                while mask != 0 {
                    let lane = (mask.trailing_zeros() / 8) as usize;
                    check_position(window, off + lane, base, archs, fatbin_offsets, candidates)?;
                    mask &= mask - 1;
                }
            }
        }
        i += 32;
    }
    while i < scan_end {
        check_position(window, i, base, archs, fatbin_offsets, candidates)?;
        i += 1;
    }
    Ok(())
}

/// Stream the actual file identity before trusting the table/cache. Never trust a catalog directory name.
/// At most 512 MiB per linear pass plus 1 MiB of fatbin headers; ~64 KiB scan memory; bounded hash cache.
pub fn inspect<R: Read + Seek>(r: &mut R) -> io::Result<ModelCompatibility> {
    r.seek(SeekFrom::Start(0))?;
    let mut hash = Sha256::new();
    let mut len = 0u64;
    let mut buf = [0u8; 65536 + 63];
    let mut carry = 0usize;
    let mut base = 0u64;
    let mut archs = BTreeSet::new();
    let mut fatbin_offsets = Vec::new();
    let mut elf_candidates = 0usize;

    loop {
        let n = r.read(&mut buf[carry..carry + 65536])?;
        if n == 0 {
            let tail_end = carry.saturating_sub(3);
            scan_window(
                &buf[..carry],
                tail_end,
                base,
                &mut archs,
                &mut fatbin_offsets,
                &mut elf_candidates,
            )?;
            break;
        }
        len += n as u64;
        if len > MAX_MODEL_BYTES {
            return Err(invalid());
        }
        let total = carry + n;
        hash.update(&buf[carry..total]);
        let scan_end = total.saturating_sub(63);
        scan_window(
            &buf[..total],
            scan_end,
            base,
            &mut archs,
            &mut fatbin_offsets,
            &mut elf_candidates,
        )?;
        let keep = total.min(63);
        buf.copy_within(total - keep..total, 0);
        carry = keep;
        base = len - keep as u64;
    }

    let hash = format!("{:x}", hash.finalize());
    if let Some((_, _, known_archs)) = KNOWN.iter().find(|(h, s, _)| *h == hash && *s == len) {
        return Ok(ModelCompatibility {
            archs: known_archs.to_vec(),
            known: true,
        });
    }
    static CACHE: OnceLock<Mutex<BTreeMap<String, ModelCompatibility>>> = OnceLock::new();
    let cache = CACHE.get_or_init(Default::default);
    if let Some(hit) = cache.lock().unwrap_or_else(|e| e.into_inner()).get(&hash) {
        return Ok(hit.clone());
    }

    let mut budget = 16384;
    let mut candidates = elf_candidates;
    for fb_pos in fatbin_offsets {
        if fb_pos + 16 <= len {
            candidates += 1;
            if candidates > 4096 {
                return Err(invalid());
            }
            archs.extend(fatbin(r, fb_pos, len, &mut budget)?);
        }
    }

    let info = ModelCompatibility {
        archs: archs.into_iter().collect(),
        known: false,
    };
    let mut cache = cache.lock().unwrap_or_else(|e| e.into_inner());
    if cache.len() >= 128 {
        cache.clear();
    }
    cache.insert(hash, info.clone());
    Ok(info)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    fn elf(sm: u16, abi8: bool) -> Vec<u8> {
        let mut b = vec![0; 64];
        b[..4].copy_from_slice(b"\x7fELF");
        b[4..9].copy_from_slice(&[2, 1, 1, if abi8 { 0x41 } else { 0x33 }, if abi8 { 8 } else { 7 }]);
        b[16..18].copy_from_slice(&2u16.to_le_bytes());
        b[18..20].copy_from_slice(&190u16.to_le_bytes());
        b[20..24].copy_from_slice(&1u32.to_le_bytes());
        let flags = if abi8 { u32::from(sm) << 8 | 2 } else { u32::from(sm) };
        b[48..52].copy_from_slice(&flags.to_le_bytes());
        b[52..54].copy_from_slice(&64u16.to_le_bytes());
        b
    }
    #[test]
    fn elf_abis_overlap_and_unknown_is_not_support() {
        let mut b = vec![0; 65530];
        b.extend(elf(86, false));
        b.extend(elf(120, true));
        let info = inspect(&mut Cursor::new(&b)).unwrap();
        assert_eq!(info.archs, [86, 120]);
        assert!(!info.known);
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12000").remove(0);
        assert_eq!(info.for_gpu(Some(&gpu)), Compatibility::Unknown);
        for n in 0..64 {
            let result = inspect(&mut Cursor::new(&elf(86, false)[..n])).unwrap();
            assert!(result.archs.is_empty());
        }
    }
    #[test]
    fn compressed_fatbin_headers_and_corruption() {
        let mut b = vec![0; 16 + 64 + 8];
        b[..8].copy_from_slice(&[0x50, 0xed, 0x55, 0xba, 1, 0, 16, 0]);
        b[8..16].copy_from_slice(&72u64.to_le_bytes());
        b[16..20].copy_from_slice(&[2, 0, 1, 1]);
        b[20..24].copy_from_slice(&64u32.to_le_bytes());
        b[24..32].copy_from_slice(&8u64.to_le_bytes());
        b[44..48].copy_from_slice(&75u32.to_le_bytes());
        assert_eq!(inspect(&mut Cursor::new(&b)).unwrap().archs, [75]);
        b[24..32].copy_from_slice(&u64::MAX.to_le_bytes());
        assert!(inspect(&mut Cursor::new(&b)).unwrap().archs.is_empty());
    }
    #[test]
    fn known_hashes_gpu_rules_and_no_name_guessing() {
        for (cap, expected) in [
            ("7.5", Compatibility::Incompatible),
            ("8.6", Compatibility::Incompatible),
            ("8.9", Compatibility::Incompatible),
            ("12.0", Compatibility::Compatible),
            ("N/A", Compatibility::Unknown),
        ] {
            let gpu = crate::gpu::parse_nvidia_smi_csv(&format!("RTX 4090,{cap},1,24000")).remove(0);
            assert_eq!(
                ModelCompatibility {
                    archs: vec![120],
                    known: true
                }
                .for_gpu(Some(&gpu)),
                expected
            );
        }
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 4090,8.9,1,24000").remove(0);
        assert_eq!(
            ModelCompatibility {
                archs: vec![86],
                known: true
            }
            .for_gpu(Some(&gpu)),
            Compatibility::Compatible
        );
        assert_eq!(KNOWN.len(), 9);
    }
}
