//! Bounded PE export reader for ReShade's optional exported NAME pointer. Never loads code.
fn u16_at(b: &[u8], n: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(n..n.checked_add(2)?)?.try_into().ok()?))
}
fn u32_at(b: &[u8], n: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(n..n.checked_add(4)?)?.try_into().ok()?))
}
fn u64_at(b: &[u8], n: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(n..n.checked_add(8)?)?.try_into().ok()?))
}
fn string(b: &[u8], at: usize) -> Option<String> {
    let end = b.get(at..)?.iter().take(1024).position(|b| *b == 0)?;
    let s = std::str::from_utf8(b.get(at..at + end)?).ok()?;
    (!s.contains(['\r', '\n', '\0'])).then(|| s.to_owned())
}
pub(crate) fn name(b: &[u8]) -> Option<String> {
    if b.get(..2)? != b"MZ" {
        return None;
    }
    let pe = u32_at(b, 60)? as usize;
    if b.get(pe..pe.checked_add(4)?)? != b"PE\0\0" {
        return None;
    }
    let count = usize::from(u16_at(b, pe + 6)?);
    if count > 96 {
        return None;
    }
    let optional = pe + 24;
    let length = usize::from(u16_at(b, pe + 20)?);
    let is64 = u16_at(b, optional)? == 0x20b;
    if !is64 && u16_at(b, optional)? != 0x10b {
        return None;
    }
    let dd = if is64 { 112 } else { 96 };
    if length < dd + 8 {
        return None;
    }
    let image = if is64 {
        u64_at(b, optional + 24)?
    } else {
        u64::from(u32_at(b, optional + 28)?)
    };
    let sections = b.get(optional + length..(optional + length).checked_add(count * 40)?)?;
    let offset = |rva: u32| -> Option<usize> {
        let mut out = None;
        for s in sections.as_chunks::<40>().0 {
            if let Some(delta) = rva.checked_sub(u32_at(s, 12)?)
                && delta < u32_at(s, 16)?
            {
                if out.is_some() {
                    return None;
                }
                out = Some(u32_at(s, 20)?.checked_add(delta)? as usize);
            }
        }
        out
    };
    let export = offset(u32_at(b, optional + dd)?)?;
    let functions = u32_at(b, export + 20)? as usize;
    let names = u32_at(b, export + 24)? as usize;
    if names > 8192 || functions > 8192 {
        return None;
    }
    let f = offset(u32_at(b, export + 28)?)?;
    let n = offset(u32_at(b, export + 32)?)?;
    let o = offset(u32_at(b, export + 36)?)?;
    for i in 0..names {
        if string(b, offset(u32_at(b, n + i * 4)?)?)? != "NAME" {
            continue;
        }
        let ordinal = usize::from(u16_at(b, o + i * 2)?);
        if ordinal >= functions {
            return None;
        }
        let at = offset(u32_at(b, f + ordinal * 4)?)?;
        let pointer = if is64 {
            u64_at(b, at)?
        } else {
            u64::from(u32_at(b, at)?)
        };
        return string(b, offset(u32::try_from(pointer.checked_sub(image)?).ok()?)?);
    }
    None
}
