//! Strict three-level RT_RCDATA reader. Never loads or executes the user's PE.
use super::{Error, Result};
const HIGH: u32 = 0x8000_0000;
fn invalid() -> Error {
    Error::Failed("Invalid or unsupported LSFG PE resources".into())
}
fn bytes(b: &[u8], p: usize, n: usize) -> Result<&[u8]> {
    b.get(p..p.checked_add(n).ok_or_else(invalid)?).ok_or_else(invalid)
}
fn u16_at(b: &[u8], p: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(bytes(b, p, 2)?.try_into().unwrap()))
}
fn u32_at(b: &[u8], p: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(bytes(b, p, 4)?.try_into().unwrap()))
}
fn entries(b: &[u8], p: usize) -> Result<Vec<(u32, u32)>> {
    bytes(b, p, 16)?;
    let n = usize::from(u16_at(b, p + 12)?) + usize::from(u16_at(b, p + 14)?);
    if n > 256 {
        return Err(invalid());
    }
    (0..n)
        .map(|i| Ok((u32_at(b, p + 16 + i * 8)?, u32_at(b, p + 20 + i * 8)?)))
        .collect()
}
fn directory(p: u32) -> Result<usize> {
    if p & HIGH == 0 {
        return Err(invalid());
    }
    Ok((p & !HIGH) as usize)
}
/// Exactly ids 0..98; bounded names, section mappings, language variants and SPIR-V instructions.
pub fn extract(b: &[u8]) -> Result<Vec<Vec<u32>>> {
    if bytes(b, 0, 2)? != b"MZ" || b.len() > super::MAX_DLL {
        return Err(invalid());
    }
    let pe = u32_at(b, 60)? as usize;
    if pe > 16 * 1024 * 1024 || bytes(b, pe, 4)? != b"PE\0\0" {
        return Err(invalid());
    }
    let n = usize::from(u16_at(b, pe + 6)?);
    if !(1..=96).contains(&n) {
        return Err(invalid());
    }
    let olen = usize::from(u16_at(b, pe + 20)?);
    if olen > 4096 {
        return Err(invalid());
    }
    let opt = bytes(b, pe + 24, olen)?;
    let d = match u16_at(opt, 0)? {
        0x10b => 96,
        0x20b => 112,
        _ => return Err(invalid()),
    };
    if u32_at(opt, d - 4)? < 3 {
        return Err(invalid());
    }
    let rva = u32_at(opt, d + 16)?;
    let size = u32_at(opt, d + 20)?;
    if rva == 0 || !(16..=64 * 1024 * 1024).contains(&size) {
        return Err(invalid());
    }
    let table = bytes(b, pe + 24 + olen, n * 40)?;
    let mut resource = None;
    for s in table.as_chunks::<40>().0 {
        let va = u32_at(s, 12)?;
        if let Some(delta) = rva.checked_sub(va)
            && delta
                .checked_add(size)
                .is_some_and(|end| end <= u32_at(s, 16).unwrap_or(0))
        {
            if resource.is_some() {
                return Err(invalid());
            }
            let start = (u32_at(s, 20)? as usize)
                .checked_add(delta as usize)
                .ok_or_else(invalid)?;
            resource = Some(bytes(b, start, size as usize)?);
        }
    }
    let r = resource.ok_or_else(invalid)?;
    let roots: Vec<_> = entries(r, 0)?.into_iter().filter(|(id, _)| *id == 10).collect();
    if roots.len() != 1 {
        return Err(invalid());
    }
    let mut result = vec![None; 99];
    for (name, langs) in entries(r, directory(roots[0].1)?)? {
        let id = if name & HIGH != 0 {
            let p = (name & !HIGH) as usize;
            let len = usize::from(u16_at(r, p)?);
            if len == 0 || len > 3 {
                return Err(invalid());
            }
            let mut id = 0u32;
            for chunk in bytes(r, p + 2, len * 2)?.as_chunks::<2>().0 {
                let c = u16::from_le_bytes(*chunk);
                if !(48..=57).contains(&c) {
                    return Err(invalid());
                }
                id = id * 10 + u32::from(c - 48);
            }
            id
        } else {
            name
        };
        if id >= 99 || result[id as usize].is_some() {
            return Err(invalid());
        }
        let languages = entries(r, directory(langs)?)?;
        if languages.is_empty() || languages.len() > 16 {
            return Err(invalid());
        }
        let mut module: Option<Vec<u32>> = None;
        for (_, leaf) in languages {
            if leaf & HIGH != 0 {
                return Err(invalid());
            }
            let p = leaf as usize;
            bytes(r, p, 16)?;
            let off = u32_at(r, p)?.checked_sub(rva).ok_or_else(invalid)? as usize;
            let len = u32_at(r, p + 4)? as usize;
            if !(20..=4 * 1024 * 1024).contains(&len) || !len.is_multiple_of(4) {
                return Err(invalid());
            }
            let words: Vec<u32> = bytes(r, off, len)?
                .as_chunks::<4>()
                .0
                .iter()
                .map(|c| u32::from_le_bytes(*c))
                .collect();
            if words[0] != 0x07230203 || words[3] > 1_000_000 || words[4] != 0 {
                return Err(invalid());
            }
            let mut at = 5;
            while at < words.len() {
                let count = (words[at] >> 16) as usize;
                if count == 0 || count > words.len() - at {
                    return Err(invalid());
                }
                at += count;
            }
            if module.as_ref().is_some_and(|m| m != &words) {
                return Err(invalid());
            }
            module = Some(words);
        }
        result[id as usize] = module;
    }
    result.into_iter().map(|m| m.ok_or_else(invalid)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    fn put(b: &mut [u8], p: usize, v: u32) {
        b[p..p + 4].copy_from_slice(&v.to_le_bytes());
    }
    pub(super) fn fixture() -> Vec<u8> {
        let mut b = vec![0; 512 + 8192];
        b[..2].copy_from_slice(b"MZ");
        put(&mut b, 60, 64);
        b[64..68].copy_from_slice(b"PE\0\0");
        b[70] = 1;
        b[84] = 240;
        b[88..90].copy_from_slice(&0x20bu16.to_le_bytes());
        put(&mut b, 196, 16);
        put(&mut b, 216, 0x1000);
        put(&mut b, 220, 8192);
        put(&mut b, 340, 0x1000);
        put(&mut b, 344, 8192);
        put(&mut b, 348, 512);
        let r = &mut b[512..];
        r[14] = 1;
        put(r, 16, 10);
        put(r, 20, HIGH | 32);
        r[46] = 99;
        for id in 0..99usize {
            put(r, 48 + id * 8, id as u32);
            put(r, 52 + id * 8, HIGH | (1024 + id * 32) as u32);
            let lang = 1024 + id * 32;
            r[lang + 14] = 1;
            put(r, lang + 16, 1033);
            put(r, lang + 20, (4200 + id * 16) as u32);
            put(r, 4200 + id * 16, (0x1000 + 6000 + id * 20) as u32);
            put(r, 4204 + id * 16, 20);
            put(r, 6000 + id * 20, 0x07230203);
            put(r, 6004 + id * 20, 0x10000);
            put(r, 6012 + id * 20, 1);
        }
        b
    }
    #[test]
    fn all_ids_and_truncated_inputs() {
        let b = fixture();
        assert_eq!(extract(&b).unwrap().len(), 99);
        for n in [0, 64, 88, 200, 512, 1000, b.len() - 1] {
            assert!(extract(&b[..n]).is_err());
        }
    }
    #[test]
    fn rejects_duplicates_cycles_out_of_range_and_conflicting_language() {
        for (at, value) in [
            (512 + 56, 0),
            (512 + 20, HIGH),
            (512 + 52, 0),
            (512 + 48, 99),
            (512 + 4204, u32::MAX),
            (512 + 4200, 0xfffffff0),
        ] {
            let mut b = fixture();
            put(&mut b, at, value);
            assert!(extract(&b).is_err(), "{at}");
        }
    }
}
