//! Read-only same-frame captures. Display metrics describe the SDR preview, not raw HDR error.
//!
//! PNG code values are left alone (16-bit PNG is reduced to 8-bit for display). PFM is linear RGB:
//! ACES fitted + sRGB, using one exposure for both sides. `white_point` is NR metadata, not exposure.
pub use image::RgbaImage;
use std::{
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

const MAX_PIXELS: u64 = 32 * 1024 * 1024;
const MAX_METADATA: u64 = 1024 * 1024;

#[derive(Clone, Debug)]
pub struct Pair {
    pub stem: String,
    pub original: PathBuf,
    pub processed: PathBuf,
    pub sidecar: Option<PathBuf>,
    pub hdr: bool,
    pub provenance: Vec<(String, String)>,
}
/// Linear exposure multiplier, NOT exposure stops. Obtain it with `load_pair` for matched HDR images.
#[derive(Clone, Copy, Debug)]
pub struct Display {
    pub exposure: f32,
}
impl Default for Display {
    fn default() -> Self {
        Self { exposure: 1.0 }
    }
}
#[derive(Clone, Copy, Debug)]
pub struct Stats {
    /// RGB PSNR in dB on 8-bit display codes; infinity for identical RGB. Alpha is ignored.
    pub psnr: f64,
    /// Mean absolute RGB difference normalized to 0..1.
    pub mean_absolute_difference: f64,
    /// Pixels with absolute Rec.709 luma-code difference > 0.05, as a percentage in 0..100.
    pub changed_percent: f64,
}
pub struct LoadedPair {
    pub original: RgbaImage,
    pub processed: RgbaImage,
    pub display: Display,
}
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

/// Read-only guidance for an empty capture folder. Unknown/unreadable config is not proof of an
/// uninstalled game; the UI only suggests installation when the INI is actually absent.
#[derive(Clone, Debug, Default)]
pub struct CaptureHelp {
    pub configuration: Option<bool>,
    pub menu_key: Option<String>,
    pub menu_disabled: bool,
    pub shortcut_disabled: bool,
}
pub fn capture_help(target: &Path) -> CaptureHelp {
    let target = if target.file_name().is_some_and(|name| name == "retoque-captures") {
        target.parent().unwrap_or(target)
    } else {
        target
    };
    let file = match File::open(target.join("OptiScaler.ini")) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            return CaptureHelp {
                configuration: Some(false),
                ..CaptureHelp::default()
            };
        }
        Err(_) => return CaptureHelp::default(),
    };
    let mut bytes = Vec::new();
    if file.take(MAX_METADATA + 1).read_to_end(&mut bytes).is_err() || bytes.len() as u64 > MAX_METADATA {
        return CaptureHelp::default();
    }
    let ini = retoque_core::ini::Ini::parse(&bytes);
    let raw = ini.get("Menu", "ShortcutKey").unwrap_or("auto").trim();
    let key = if raw.eq_ignore_ascii_case("auto") {
        Some(0x2d)
    } else if let Some(hex) = raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X")) {
        u32::from_str_radix(hex, 16).ok()
    } else {
        raw.parse::<u32>().ok()
    };
    let menu_key = match key {
        Some(0x2d) => Some("Insert".to_owned()),
        Some(key @ 0x70..=0x87) => Some(format!("F{}", key - 0x70 + 1)),
        Some(key @ (0x30..=0x39 | 0x41..=0x5a)) => char::from_u32(key).map(|key| key.to_string()),
        _ => None,
    };
    CaptureHelp {
        configuration: Some(true),
        menu_key,
        shortcut_disabled: raw == "-1",
        menu_disabled: ini
            .get("Menu", "OverlayMenu")
            .is_some_and(|value| value.eq_ignore_ascii_case("false")),
    }
}

/// Find complete pairs under `<target>/retoque-captures` (or in that directory itself).
/// Capture stems begin with a UTC timestamp, so descending names give newest captures first.
/// Use `scan` when a missing directory must be distinguished from a permission/I/O failure.
pub fn find(target: &Path) -> Vec<Pair> {
    scan(target).unwrap_or_default()
}

pub fn scan(target: &Path) -> io::Result<Vec<Pair>> {
    let dir = if target.file_name().is_some_and(|n| n == "retoque-captures") {
        target.to_path_buf()
    } else {
        target.join("retoque-captures")
    };
    let entries = match fs::read_dir(&dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(vec![]),
        Err(e) => return Err(e),
    };
    let mut pairs = vec![];
    for entry in entries {
        let entry = entry?;
        let name = entry.file_name();
        let Some(name) = name.to_str() else { continue };
        let Some((stem, ext)) = name
            .strip_suffix(".original.png")
            .map(|s| (s, "png"))
            .or_else(|| name.strip_suffix(".original.pfm").map(|s| (s, "pfm")))
        else {
            continue;
        };
        let original = entry.path();
        let processed = dir.join(format!("{stem}.processed.{ext}"));
        if !original.is_file() || !processed.is_file() {
            continue;
        }
        let sidecar_path = dir.join(format!("{stem}.json"));
        let sidecar = sidecar_path.is_file().then_some(sidecar_path);
        let provenance = sidecar
            .as_ref()
            .and_then(|p| json_provenance(p).ok())
            .or_else(|| (ext == "png").then(|| png_provenance(&original).ok()).flatten())
            .unwrap_or_default();
        pairs.push(Pair {
            stem: stem.into(),
            original,
            processed,
            sidecar,
            hdr: ext == "pfm",
            provenance,
        });
    }
    pairs.sort_by(|a, b| b.stem.cmp(&a.stem).then_with(|| a.original.cmp(&b.original)));
    Ok(pairs)
}
fn json_provenance(path: &Path) -> io::Result<Vec<(String, String)>> {
    let f = File::open(path)?;
    if f.metadata()?.len() > MAX_METADATA {
        return Err(invalid("Capture metadata exceeds 1 MiB"));
    }
    let value: serde_json::Value = serde_json::from_reader(f.take(MAX_METADATA)).map_err(io::Error::other)?;
    let object = value
        .as_object()
        .ok_or_else(|| invalid("Capture metadata is not an object"))?;
    Ok(object
        .iter()
        .map(|(key, value)| {
            (
                key.clone(),
                value.as_str().map(str::to_owned).unwrap_or_else(|| value.to_string()),
            )
        })
        .collect())
}
/// Skip image chunks without allocating or inflating IDAT, including metadata written after IDAT.
fn png_provenance(path: &Path) -> io::Result<Vec<(String, String)>> {
    let mut f = BufReader::new(File::open(path)?);
    let file_len = f.get_ref().metadata()?.len();
    let mut signature = [0; 8];
    f.read_exact(&mut signature)?;
    if signature != *b"\x89PNG\r\n\x1a\n" {
        return Err(invalid("Invalid PNG signature"));
    }
    let mut total_text = 0;
    loop {
        let mut header = [0; 8];
        f.read_exact(&mut header)?;
        let len = u32::from_be_bytes(header[..4].try_into().unwrap()) as u64;
        let end = f
            .stream_position()?
            .checked_add(len + 4)
            .ok_or_else(|| invalid("PNG chunk overflow"))?;
        if end > file_len {
            return Err(invalid("Truncated PNG chunk"));
        }
        if &header[4..] == b"tEXt" {
            total_text += len;
            if total_text > MAX_METADATA {
                return Err(invalid("PNG text exceeds 1 MiB"));
            }
            let mut data = vec![0; len as usize];
            f.read_exact(&mut data)?;
            if let Some(text) = data.strip_prefix(b"Retoque.Provenance\0") {
                let text: String = text.iter().map(|b| char::from(*b)).collect();
                return Ok(text
                    .split(';')
                    .filter_map(|item| item.split_once('='))
                    .map(|(k, v)| (k.trim().into(), v.trim().into()))
                    .collect());
            }
        }
        if &header[4..] == b"IEND" {
            return Ok(vec![]);
        }
        f.seek(SeekFrom::Start(end))?;
    }
}

struct LinearImage {
    width: u32,
    height: u32,
    rgb: Vec<f32>,
}
/// PFM header lines are bounded so corrupt files cannot turn metadata parsing into an unbounded allocation.
fn header_line(reader: &mut impl BufRead) -> io::Result<String> {
    loop {
        let mut line = String::new();
        let read = reader.take(4097).read_line(&mut line)?;
        if read == 0 || read > 4096 || !line.ends_with('\n') {
            return Err(invalid("Invalid PFM header"));
        }
        let line = line.trim();
        if !line.is_empty() && !line.starts_with('#') {
            return Ok(line.into());
        }
    }
}
struct PfmHeader {
    width: u32,
    height: u32,
    scale: f32,
    pixels_start: u64,
}
fn pfm_header(reader: &mut BufReader<File>) -> io::Result<PfmHeader> {
    if header_line(reader)? != "PF" {
        return Err(invalid("Only RGB PFM is supported"));
    }
    let dimensions = header_line(reader)?;
    let dims = dimensions
        .split_whitespace()
        .map(str::parse::<u32>)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| invalid("Invalid PFM dimensions"))?;
    if dims.len() != 2 {
        return Err(invalid("Invalid PFM dimensions"));
    }
    let (width, height) = (dims[0], dims[1]);
    let pixels = u64::from(width) * u64::from(height);
    if pixels == 0 || pixels > MAX_PIXELS || width > 16384 || height > 16384 {
        return Err(invalid("PFM dimensions exceed the display limit"));
    }
    let scale: f32 = header_line(reader)?.parse().map_err(|_| invalid("Invalid PFM scale"))?;
    if !scale.is_finite() || scale == 0.0 {
        return Err(invalid("Invalid PFM scale"));
    }
    if reader
        .get_ref()
        .metadata()?
        .len()
        .saturating_sub(reader.stream_position()?)
        < pixels * 12
    {
        return Err(invalid("Truncated PFM pixels"));
    }
    Ok(PfmHeader {
        width,
        height,
        scale,
        pixels_start: reader.stream_position()?,
    })
}
fn read_pfm(path: &Path) -> io::Result<LinearImage> {
    let mut reader = BufReader::new(File::open(path)?);
    let PfmHeader {
        width, height, scale, ..
    } = pfm_header(&mut reader)?;
    let pixels = u64::from(width) * u64::from(height);
    let row_samples = width as usize * 3;
    let mut rgb = vec![0.0; pixels as usize * 3];
    let mut row = vec![0; row_samples * 4];
    for y in (0..height as usize).rev() {
        reader.read_exact(&mut row)?;
        for (pixel, bytes) in rgb[y * row_samples..(y + 1) * row_samples]
            .iter_mut()
            .zip(row.as_chunks::<4>().0.iter())
        {
            let bytes = *bytes;
            *pixel = if scale < 0.0 {
                f32::from_le_bytes(bytes)
            } else {
                f32::from_be_bytes(bytes)
            } * scale.abs();
        }
    }
    Ok(LinearImage { width, height, rgb })
}
fn explicit_exposure(pair: &Pair) -> Option<f32> {
    ["display_exposure", "exposure", "exposure_multiplier"]
        .into_iter()
        .find_map(|key| {
            pair.provenance
                .iter()
                .find(|(k, _)| k == key)
                .and_then(|(_, v)| v.parse::<f32>().ok())
                .filter(|v| v.is_finite() && *v > 0.0)
        })
}
fn auto_exposure(image: &LinearImage) -> f32 {
    // Sample the whole original with bounded scratch. P70 is less sensitive to large dark areas
    // than a geometric mean. ACES(0.13) is about 0.18 linear display grey (0.46 sRGB), not ACES(0.18).
    let pixels = image.rgb.len() / 3;
    let count = pixels.min(65536);
    let mut luminance = Vec::with_capacity(count);
    for i in 0..count {
        let offset = (i * pixels / count) * 3;
        let rgb = &image.rgb[offset..offset + 3];
        let l = 0.2126 * rgb[0].max(0.0) + 0.7152 * rgb[1].max(0.0) + 0.0722 * rgb[2].max(0.0);
        if l.is_finite() && l > 1e-8 {
            luminance.push(l);
        }
    }
    if luminance.is_empty() {
        return 1.0;
    }
    let rank = ((luminance.len() - 1) as f64 * 0.70) as usize;
    let (_, key, _) = luminance.select_nth_unstable_by(rank, f32::total_cmp);
    (0.13 / *key).clamp(1e-6, 1e6)
}

fn display_byte(value: f32, exposure: f32) -> u8 {
    if value.is_nan() || value <= 0.0 {
        return 0;
    }
    if value.is_infinite() {
        return 255;
    }
    let x = (f64::from(value) * f64::from(exposure)).min(1e10);
    let linear = ((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)).clamp(0.0, 1.0);
    let srgb = if linear <= 0.0031308 {
        linear * 12.92
    } else {
        1.055 * linear.powf(1.0 / 2.4) - 0.055
    };
    (srgb * 255.0).round() as u8
}
fn tonemap(image: LinearImage, display: Display) -> RgbaImage {
    let mut out = RgbaImage::new(image.width, image.height);
    for (dst, src) in out
        .as_mut()
        .as_chunks_mut::<4>()
        .0
        .iter_mut()
        .zip(image.rgb.as_chunks::<3>().0.iter())
    {
        for c in 0..3 {
            dst[c] = display_byte(src[c], display.exposure);
        }
        dst[3] = 255;
    }
    out
}
fn is_pfm(path: &Path) -> bool {
    path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("pfm"))
}
/// Decode one file with an explicitly shared display transform. Errors preserve I/O/decode failure.
pub fn load(path: &Path, display: Display) -> io::Result<RgbaImage> {
    if is_pfm(path) {
        if !display.exposure.is_finite() || display.exposure <= 0.0 {
            return Err(invalid("Exposure must be positive and finite"));
        }
        return Ok(tonemap(read_pfm(path)?, display));
    }
    let mut reader = image::ImageReader::with_format(BufReader::new(File::open(path)?), image::ImageFormat::Png);
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(16384);
    limits.max_image_height = Some(16384);
    limits.max_alloc = Some(MAX_PIXELS * 16);
    reader.limits(limits);
    Ok(reader.decode().map_err(io::Error::other)?.into_rgba8())
}
/// Decode the original once and choose exposure from its luminance or an explicit sidecar multiplier.
/// The processed image can never influence this exposure.
pub fn load_original(pair: &Pair) -> io::Result<(RgbaImage, Display)> {
    if !pair.hdr {
        return Ok((load(&pair.original, Display::default())?, Display::default()));
    }
    let original = read_pfm(&pair.original)?;
    let display = Display {
        exposure: explicit_exposure(pair).unwrap_or_else(|| auto_exposure(&original)),
    };
    Ok((tonemap(original, display), display))
}
pub fn load_pair(pair: &Pair) -> io::Result<LoadedPair> {
    load_pair_with_ev(pair, 0.0)
}

/// Exposure adjustment in stops, relative to the same automatic/recorded base for both images.
/// PNG remains display-referred; EV only changes HDR PFM pairs.
pub fn load_pair_with_ev(pair: &Pair, ev: f32) -> io::Result<LoadedPair> {
    if !ev.is_finite() || !(-4.0..=4.0).contains(&ev) {
        return Err(invalid("Exposure adjustment must be between -4 and +4 EV"));
    }
    let (original, display) = if pair.hdr {
        let linear = read_pfm(&pair.original)?;
        let display = Display {
            exposure: explicit_exposure(pair).unwrap_or_else(|| auto_exposure(&linear)) * ev.exp2(),
        };
        (tonemap(linear, display), display)
    } else {
        load_original(pair)?
    };
    let processed = load(&pair.processed, display)?;
    matching(&original, &processed)?;
    Ok(LoadedPair {
        original,
        processed,
        display,
    })
}

/// Small original preview for the pair list. PFM reads only sampled source rows and stores at most
/// 384x256 floats, so a thumbnail never runs the full 3440x1440 tonemapping/metrics pipeline.
/// PNG's lossless codec must decode the source first; only the resized result is retained by callers.
pub fn load_thumbnail(pair: &Pair) -> io::Result<RgbaImage> {
    let original = if pair.hdr {
        let mut reader = BufReader::new(File::open(&pair.original)?);
        let h = pfm_header(&mut reader)?;
        let ratio = (384.0 / h.width as f32).min(256.0 / h.height as f32).min(1.0);
        let width = (h.width as f32 * ratio).round().max(1.0) as u32;
        let height = (h.height as f32 * ratio).round().max(1.0) as u32;
        let mut rgb = vec![0.0; width as usize * height as usize * 3];
        let mut row = vec![0; h.width as usize * 12];
        for y in 0..height {
            let source_y = ((u64::from(y) * 2 + 1) * u64::from(h.height) / (u64::from(height) * 2)) as u32;
            reader.seek(SeekFrom::Start(
                h.pixels_start + u64::from(h.height - 1 - source_y) * u64::from(h.width) * 12,
            ))?;
            reader.read_exact(&mut row)?;
            for x in 0..width {
                let source_x = ((u64::from(x) * 2 + 1) * u64::from(h.width) / (u64::from(width) * 2)) as usize;
                for c in 0..3 {
                    let offset = source_x * 12 + c * 4;
                    let bytes = row[offset..offset + 4].try_into().unwrap();
                    rgb[(y as usize * width as usize + x as usize) * 3 + c] = if h.scale < 0.0 {
                        f32::from_le_bytes(bytes)
                    } else {
                        f32::from_be_bytes(bytes)
                    } * h.scale.abs();
                }
            }
        }
        let image = LinearImage { width, height, rgb };
        let display = Display {
            exposure: explicit_exposure(pair).unwrap_or_else(|| auto_exposure(&image)),
        };
        tonemap(image, display)
    } else {
        load(&pair.original, Display::default())?
    };
    let ratio = (168.0 / original.width() as f32)
        .min(80.0 / original.height() as f32)
        .min(1.0);
    Ok(image::imageops::thumbnail(
        &original,
        (original.width() as f32 * ratio).round().max(1.0) as u32,
        (original.height() as f32 * ratio).round().max(1.0) as u32,
    ))
}

fn matching(a: &RgbaImage, b: &RgbaImage) -> io::Result<()> {
    if a.dimensions() != b.dimensions() || a.width() == 0 || a.height() == 0 {
        return Err(invalid("Comparison requires equal, nonempty image dimensions"));
    }
    Ok(())
}
pub fn stats(a: &RgbaImage, b: &RgbaImage) -> io::Result<Stats> {
    matching(a, b)?;
    let (mut squared, mut absolute, mut changed) = (0.0, 0.0, 0_u64);
    for (a, b) in a
        .as_raw()
        .as_chunks::<4>()
        .0
        .iter()
        .zip(b.as_raw().as_chunks::<4>().0.iter())
    {
        let mut luma_delta = 0.0;
        for c in 0..3 {
            let delta = (f64::from(a[c]) - f64::from(b[c])) / 255.0;
            squared += delta * delta;
            absolute += delta.abs();
            luma_delta += delta * [0.2126, 0.7152, 0.0722][c];
        }
        changed += u64::from(luma_delta.abs() > 0.05);
    }
    let pixels = f64::from(a.width()) * f64::from(a.height());
    let mse = squared / (pixels * 3.0);
    Ok(Stats {
        psnr: if mse == 0.0 { f64::INFINITY } else { -10.0 * mse.log10() },
        mean_absolute_difference: absolute / (pixels * 3.0),
        changed_percent: changed as f64 * 100.0 / pixels,
    })
}
/// Black = equal RGB, blue/cyan = small differences, yellow/red = large differences.
/// A fixed 8x gain makes subtle errors visible without normalizing each image independently.
pub fn diff_image(a: &RgbaImage, b: &RgbaImage) -> io::Result<RgbaImage> {
    matching(a, b)?;
    let mut out = RgbaImage::new(a.width(), a.height());
    for ((a, b), dst) in a
        .as_raw()
        .as_chunks::<4>()
        .0
        .iter()
        .zip(b.as_raw().as_chunks::<4>().0.iter())
        .zip(out.as_mut().as_chunks_mut::<4>().0.iter_mut())
    {
        let d = (0..3)
            .map(|c| f32::from(a[c].abs_diff(b[c])) / 255.0)
            .fold(0.0_f32, f32::max);
        let t = (d * 8.0).min(1.0);
        dst[0] = ((t * 3.0 - 1.0).clamp(0.0, 1.0) * 255.0) as u8;
        dst[1] = ((1.0 - (t * 3.0 - 1.5).abs()).clamp(0.0, 1.0) * 255.0) as u8;
        dst[2] = ((t * 6.0).min(1.0) * (1.0 - t * 2.0).max(0.0) * 255.0) as u8;
        dst[3] = 255;
    }
    Ok(out)
}
#[cfg(test)]
mod tests {
    use super::*;
    use image::Rgba;
    use std::{fs, io::Write};
    fn pfm(path: &Path, width: u32, height: u32, scale: f32, values: &[f32]) {
        let mut f = fs::File::create(path).unwrap();
        write!(f, "PF\n{width} {height}\n{scale}\n").unwrap();
        for value in values {
            f.write_all(&if scale < 0.0 {
                value.to_le_bytes()
            } else {
                value.to_be_bytes()
            })
            .unwrap();
        }
    }
    fn pair(dir: &Path, original: f32, processed: f32) -> Pair {
        let a = dir.join("capture.original.pfm");
        let b = dir.join("capture.processed.pfm");
        pfm(&a, 1, 1, -1.0, &[original; 3]);
        pfm(&b, 1, 1, -1.0, &[processed; 3]);
        Pair {
            stem: "capture".into(),
            original: a,
            processed: b,
            sidecar: None,
            hdr: true,
            provenance: vec![],
        }
    }
    #[test]
    fn pfm_bottom_up_and_both_endian_orders() {
        let dir = tempfile::tempdir().unwrap();
        for scale in [-1.0, 1.0] {
            let p = dir.path().join("image.pfm");
            pfm(&p, 1, 2, scale, &[0., 1., 0., 1., 0., 0.]);
            let image = load(&p, Display::default()).unwrap();
            assert!(image.get_pixel(0, 0)[0] > 200);
            assert_eq!(image.get_pixel(0, 0)[1], 0);
            assert!(image.get_pixel(0, 1)[1] > 200);
            assert_eq!(image.get_pixel(0, 1)[0], 0);
        }
    }
    #[test]
    fn pfm_scale_magnitude_and_nonfinite_are_safe() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("image.pfm");
        pfm(&p, 1, 1, -2.0, &[0.25; 3]);
        let scaled = load(&p, Display::default()).unwrap();
        pfm(&p, 1, 1, -1.0, &[0.5; 3]);
        assert_eq!(scaled, load(&p, Display::default()).unwrap());
        pfm(&p, 1, 1, -1.0, &[f32::NAN, f32::INFINITY, -1.]);
        assert_eq!(
            load(&p, Display::default()).unwrap().get_pixel(0, 0).0,
            [0, 255, 0, 255]
        );
    }
    #[test]
    fn pfm_rejects_bad_headers_truncation_and_huge_dimensions() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("bad.pfm");
        for bytes in [
            b"PF\n1 1\n-1\n".as_slice(),
            b"PF\n0 1\n-1\n",
            b"PF\n99999999 99999999\n-1\n",
            b"PF\n1 1\n0\n",
            b"PF\n1 1\nNaN\n",
        ] {
            fs::write(&p, bytes).unwrap();
            assert!(load(&p, Display::default()).is_err());
        }
    }
    #[test]
    fn png_is_display_referred_and_ignores_exposure() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("image.png");
        let image = RgbaImage::from_pixel(2, 3, Rgba([12, 80, 200, 255]));
        image.save(&p).unwrap();
        assert_eq!(load(&p, Display { exposure: 50.0 }).unwrap(), image);
    }
    #[test]
    fn find_requires_both_files_and_orders_capture_stamps() {
        let dir = tempfile::tempdir().unwrap();
        let captures = dir.path().join("retoque-captures");
        fs::create_dir(&captures).unwrap();
        for stem in ["20260101-120000-test-001", "20260102-120000-test-002"] {
            for suffix in ["original.png", "processed.png"] {
                RgbaImage::new(1, 1)
                    .save(captures.join(format!("{stem}.{suffix}")))
                    .unwrap();
            }
        }
        fs::write(captures.join("orphan.original.pfm"), "PF").unwrap();
        fs::write(
            captures.join("20260102-120000-test-002.json"),
            r#"{"model_version":"test","exposure":2.0}"#,
        )
        .unwrap();
        let pairs = find(dir.path());
        assert_eq!(pairs.len(), 2);
        assert!(pairs[0].stem.starts_with("20260102"));
        assert!(pairs[0].sidecar.is_some());
        assert!(pairs[0].provenance.contains(&("exposure".into(), "2.0".into())));
        assert_eq!(find(&captures).len(), 2);
    }
    #[test]
    fn exposure_is_shared_and_explicit_exposure_wins() {
        let dir = tempfile::tempdir().unwrap();
        let mut p = pair(dir.path(), 0.01, 0.02);
        let loaded = load_pair(&p).unwrap();
        assert!(loaded.display.exposure > 1.0);
        assert!(loaded.processed.get_pixel(0, 0)[0] > loaded.original.get_pixel(0, 0)[0]);
        assert_eq!(loaded.processed, load(&p.processed, loaded.display).unwrap());
        p.provenance.push(("exposure".into(), "2".into()));
        assert_eq!(load_pair(&p).unwrap().display.exposure, 2.0);
    }
    #[test]
    fn stats_and_diff_are_normalized_and_reject_dimension_mismatch() {
        let a = RgbaImage::from_pixel(2, 1, Rgba([0, 0, 0, 255]));
        let mut b = a.clone();
        b.put_pixel(0, 0, Rgba([255, 255, 255, 255]));
        let equal = stats(&a, &a).unwrap();
        assert!(equal.psnr.is_infinite());
        assert_eq!(equal.changed_percent, 0.0);
        let delta = stats(&a, &b).unwrap();
        assert!((delta.psnr - 3.0102999566).abs() < 1e-6);
        assert_eq!(delta.mean_absolute_difference, 0.5);
        assert_eq!(delta.changed_percent, 50.0);
        assert_eq!(diff_image(&a, &a).unwrap().get_pixel(0, 0).0, [0, 0, 0, 255]);
        assert_ne!(diff_image(&a, &b).unwrap().get_pixel(0, 0).0, [0, 0, 0, 255]);
        assert!(stats(&a, &RgbaImage::new(1, 1)).is_err());
        assert!(diff_image(&a, &RgbaImage::new(0, 0)).is_err());
    }
    #[test]
    fn png_text_provenance_without_json_and_malformed_json_fallback() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("retoque-captures");
        fs::create_dir(&folder).unwrap();
        let a = folder.join("capture.original.png");
        let b = folder.join("capture.processed.png");
        RgbaImage::from_pixel(1, 1, Rgba([12, 30, 90, 255])).save(&a).unwrap();
        fs::copy(&a, &b).unwrap();
        // Append a valid Latin-1 tEXt chunk before IEND; IDAT must be skipped, not decoded.
        let mut png = fs::read(&a).unwrap();
        let text = b"Retoque.Provenance\0model_version=synthetic;route=unit-test;settings=model=2x1 scale=1";
        let mut chunk = Vec::new();
        chunk.extend_from_slice(&(text.len() as u32).to_be_bytes());
        chunk.extend_from_slice(b"tEXt");
        chunk.extend_from_slice(text);
        let mut crc = !0u32;
        for byte in &chunk[4..] {
            crc ^= u32::from(*byte);
            for _ in 0..8 {
                crc = (crc >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(crc & 1));
            }
        }
        chunk.extend_from_slice(&(!crc).to_be_bytes());
        png.splice(png.len() - 12..png.len() - 12, chunk);
        fs::write(&a, png).unwrap();
        for corrupt_json in [false, true] {
            if corrupt_json {
                fs::write(folder.join("capture.json"), "{broken").unwrap();
            }
            let pairs = find(dir.path());
            assert_eq!(pairs.len(), 1);
            assert!(
                pairs[0]
                    .provenance
                    .contains(&("model_version".into(), "synthetic".into()))
            );
            assert!(
                pairs[0]
                    .provenance
                    .contains(&("settings".into(), "model=2x1 scale=1".into()))
            );
            assert_eq!(
                load(&a, Display::default()).unwrap().get_pixel(0, 0).0,
                [12, 30, 90, 255]
            );
        }
    }
    #[test]
    fn black_hdr_and_invalid_exposure_do_not_produce_invalid_display() {
        let dir = tempfile::tempdir().unwrap();
        let mut pair = pair(dir.path(), 0.0, 0.0);
        pair.provenance.push(("exposure".into(), "NaN".into()));
        let loaded = load_pair(&pair).unwrap();
        assert_eq!(loaded.display.exposure, 1.0);
        assert_eq!(loaded.original.get_pixel(0, 0).0, [0, 0, 0, 255]);
        assert!(load(&pair.original, Display { exposure: 0.0 }).is_err());
        pfm(&pair.processed, 2, 1, -1.0, &[0.; 6]);
        assert!(load_pair(&pair).is_err());
    }
    #[test]
    fn percentile_key_resists_large_dark_areas_and_maps_to_display_midgrey() {
        let mut rgb = vec![0.0001; 60 * 3];
        rgb.extend_from_slice(&[0.1; 40 * 3]);
        let image = LinearImage {
            width: 100,
            height: 1,
            rgb,
        };
        let exposure = auto_exposure(&image);
        assert!((exposure - 1.3).abs() < 0.001, "exposure was {exposure}");
        assert!((i32::from(display_byte(0.1, exposure)) - 118).abs() <= 2);
    }

    #[test]
    fn ev_changes_both_images_by_the_same_factor_and_zero_resets() {
        let dir = tempfile::tempdir().unwrap();
        let pair = pair(dir.path(), 0.01, 0.02);
        let base = load_pair(&pair).unwrap();
        for ev in [-4.0, -1.0, 0.0, 1.0, 4.0] {
            let changed = load_pair_with_ev(&pair, ev).unwrap();
            assert!((changed.display.exposure / base.display.exposure - ev.exp2()).abs() < 1e-5);
            assert_eq!(changed.original, load(&pair.original, changed.display).unwrap());
            assert_eq!(changed.processed, load(&pair.processed, changed.display).unwrap());
        }
        assert_eq!(base.original, load_pair_with_ev(&pair, 0.0).unwrap().original);
        assert!(load_pair_with_ev(&pair, f32::NAN).is_err());
        assert!(load_pair_with_ev(&pair, 5.0).is_err());
    }
    #[test]
    fn sparse_pfm_thumbnail_preserves_rows_endianness_and_size_bound() {
        let dir = tempfile::tempdir().unwrap();
        let mut pair = pair(dir.path(), 0.01, 0.01);
        pair.provenance.push(("exposure".into(), "1".into()));
        for scale in [-1.0, 1.0] {
            pfm(&pair.original, 1, 2, scale, &[0., 1., 0., 1., 0., 0.]);
            let thumb = load_thumbnail(&pair).unwrap();
            assert_eq!(thumb, load(&pair.original, Display::default()).unwrap());
        }
        pfm(&pair.original, 800, 400, -1.0, &vec![0.1; 800 * 400 * 3]);
        let thumb = load_thumbnail(&pair).unwrap();
        assert!(thumb.width() <= 168 && thumb.height() <= 80);
    }
    #[test]
    fn capture_help_uses_real_menu_key_and_does_not_invent_an_unknown_shortcut() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(capture_help(dir.path()).configuration, Some(false));
        for (value, expected) in [
            ("auto", Some("Insert")),
            ("0x78", Some("F9")),
            ("114", Some("F3")),
            ("bad", None),
            ("-1", None),
        ] {
            fs::write(
                dir.path().join("OptiScaler.ini"),
                format!("[Menu]\nShortcutKey={value}\nOverlayMenu=false\n"),
            )
            .unwrap();
            let help = capture_help(dir.path());
            assert_eq!(help.configuration, Some(true));
            assert_eq!(help.menu_key.as_deref(), expected);
            assert_eq!(help.shortcut_disabled, value == "-1");
            assert!(help.menu_disabled);
        }
    }
}
