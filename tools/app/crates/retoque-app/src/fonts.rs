//! Chinese, Japanese and Korean glyphs from an installed system font. egui's own fonts have none and the app bundles
//! no font, so a font provider finds a CJK font the first time a character needs one: when the app is in Japanese,
//! Korean or Chinese, when the language selector shows those names, or when a game's title has them. A font file is
//! read at most once per process, and only then; finding one only checks a few fixed paths (and on Linux, when
//! those miss, asks fontconfig once, with a deadline).

use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};

use crate::i18n::Lang;

/// A writing system a CJK font has to cover.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Script {
    Japanese,
    Korean,
    Chinese,
}

impl Script {
    #[cfg(test)]
    const ALL: [Script; 3] = [Script::Japanese, Script::Korean, Script::Chinese];

    pub(crate) fn of(lang: Lang) -> Option<Script> {
        match lang {
            Lang::Ja => Some(Script::Japanese),
            Lang::Ko => Some(Script::Korean),
            Lang::ZhCn => Some(Script::Chinese),
            _ => None,
        }
    }

    /// The fonts to try for a character, in order. Han characters take the reader's regional forms (a Japanese
    /// reader gets the Japanese face); kana and Hangul need a font of their own script.
    fn for_char(c: char, reader: Lang) -> &'static [Script] {
        use Script::*;
        match c as u32 {
            0x1100..=0x11FF | 0x3130..=0x318F | 0xA960..=0xA97F | 0xAC00..=0xD7AF | 0xD7B0..=0xD7FF => &[Korean],
            0x3040..=0x30FF | 0x31F0..=0x31FF | 0xFF66..=0xFF9F => &[Japanese, Chinese],
            0x2E80..=0x2FDF
            | 0x3000..=0x303F
            | 0x3190..=0x31EF
            | 0x3200..=0x33FF
            | 0x3400..=0x4DBF
            | 0x4E00..=0x9FFF
            | 0xF900..=0xFAFF
            | 0xFE30..=0xFE4F
            | 0xFF00..=0xFF65
            | 0xFFA0..=0xFFEF
            | 0x20000..=0x3134F => match reader {
                Lang::Ja => &[Japanese, Chinese, Korean],
                Lang::Ko => &[Korean, Chinese, Japanese],
                _ => &[Chinese, Japanese, Korean],
            },
            _ => &[],
        }
    }

    /// fontconfig's name for the language.
    #[cfg(not(windows))]
    fn fc_lang(self) -> &'static str {
        match self {
            Script::Japanese => "ja",
            Script::Korean => "ko",
            Script::Chinese => "zh-cn",
        }
    }

    /// Known font files (and the face inside a collection) that cover the script, most fitting first.
    fn candidates(self) -> Vec<(PathBuf, u32)> {
        #[cfg(windows)]
        {
            let fonts = std::env::var_os("WINDIR")
                .map(PathBuf::from)
                .unwrap_or_else(|| PathBuf::from(r"C:\Windows"))
                .join("Fonts");
            let files: &[&str] = match self {
                Script::Japanese => &["YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc"],
                Script::Korean => &["malgun.ttf", "gulim.ttc"],
                Script::Chinese => &["msyh.ttc", "msyh.ttf", "simsun.ttc"],
            };
            files.iter().map(|f| (fonts.join(f), 0)).collect()
        }
        #[cfg(not(windows))]
        {
            // Noto Sans CJK collections hold one face per region: 0 Japanese, 1 Korean, 2 Simplified Chinese.
            let face = match self {
                Script::Japanese => 0,
                Script::Korean => 1,
                Script::Chinese => 2,
            };
            let mut list: Vec<(PathBuf, u32)> = [
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/noto-cjk/NotoSansCJK-VF.ttc",
                "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
            ]
            .iter()
            .map(|p| (PathBuf::from(p), face))
            .collect();
            let own: &[&str] = match self {
                Script::Japanese => &[
                    "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
                    "/usr/share/fonts/TTF/ipag.ttf",
                ],
                Script::Korean => &[
                    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                    "/usr/share/fonts/TTF/NanumGothic.ttf",
                    "/usr/share/fonts/naver-nanum/NanumGothic.ttf",
                ],
                Script::Chinese => &[],
            };
            list.extend(own.iter().map(|p| (PathBuf::from(p), 0)));
            // WenQuanYi Micro Hei covers Han, kana and Hangul.
            list.extend(
                [
                    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
                    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                ]
                .iter()
                .map(|p| (PathBuf::from(p), 0)),
            );
            list
        }
    }

    /// The font file for the script, looked up once per process.
    fn font(self) -> Option<&'static (PathBuf, u32)> {
        static FOUND: [OnceLock<Option<(PathBuf, u32)>>; 3] = [const { OnceLock::new() }; 3];
        FOUND[self as usize]
            .get_or_init(|| {
                let found = self.candidates().into_iter().find(|(path, _)| path.is_file());
                #[cfg(not(windows))]
                let found = found.or_else(|| fc_match(self));
                found
            })
            .as_ref()
    }
}

/// Whether the app can draw a language: always, except CJK without a CJK font on this computer.
pub(crate) fn can_show(lang: Lang) -> bool {
    Script::of(lang).is_none_or(|script| script.font().is_some())
}

/// Ask fontconfig for a sans-serif font that covers the script; accept it only if it does (fc-match always answers
/// with something). Bounded: a missing or stuck fc-match means no font, never a frozen window.
#[cfg(not(windows))]
fn fc_match(script: Script) -> Option<(PathBuf, u32)> {
    use std::io::Read;
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};
    let mut child = Command::new("fc-match")
        .args([
            "-f",
            "%{file}\t%{index}\t%{lang}",
            &format!("sans-serif:lang={}", script.fc_lang()),
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let deadline = Instant::now() + Duration::from_millis(1500);
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(5)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    }
    let mut out = String::new();
    child.stdout.take()?.read_to_string(&mut out).ok()?;
    parse_fc_match(&out, script)
}

#[cfg(not(windows))]
fn parse_fc_match(out: &str, script: Script) -> Option<(PathBuf, u32)> {
    let mut fields = out.trim_end().split('\t');
    let (file, index, langs) = (fields.next()?, fields.next()?, fields.next()?);
    let covers = langs.split('|').any(|l| l == script.fc_lang());
    let usable = [".ttf", ".otf", ".ttc", ".otc"]
        .iter()
        .any(|ext| file.to_ascii_lowercase().ends_with(ext));
    (covers && usable && file.starts_with('/')).then(|| (PathBuf::from(file), index.parse().unwrap_or(0)))
}

/// A font file's bytes, read the first time and shared afterwards (None if it could not be read).
fn bytes(path: &PathBuf) -> Option<egui::epaint::text::Blob> {
    // A CJK collection is 15–60 MB; a bigger file is not a font this app should hold in memory.
    const MAX_BYTES: u64 = 96 * 1024 * 1024;
    static READ: Mutex<Vec<(PathBuf, Option<egui::epaint::text::Blob>)>> = Mutex::new(Vec::new());
    let mut read = READ.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((_, blob)) = read.iter().find(|(p, _)| p == path) {
        return blob.clone();
    }
    let blob = std::fs::metadata(path)
        .ok()
        .filter(|m| m.len() <= MAX_BYTES)
        .and_then(|_| std::fs::read(path).ok())
        .map(|b| Arc::new(b) as egui::epaint::text::Blob);
    crate::applog::line(format_args!(
        "CJK font {}: {}",
        path.display(),
        if blob.is_some() { "loaded" } else { "unreadable" }
    ));
    read.push((path.clone(), blob.clone()));
    blob
}

/// Finds a CJK font for a character no installed font has. egui asks at most once per family and character,
/// appends the answer to the family, and skips (with a log line) a file it cannot parse or that lacks the glyph.
pub(crate) struct CjkFonts {
    /// The app's language: Han characters use its regional face.
    pub(crate) reader: Lang,
}

impl egui::FontProvider for CjkFonts {
    fn font_for(&self, request: &egui::FallbackRequest<'_>) -> Option<egui::FontInsert> {
        let c = request.cluster.chars().next()?;
        let (path, index) = Script::for_char(c, self.reader)
            .iter()
            .find_map(|script| script.font())?;
        let blob = bytes(path)?;
        let families = [egui::FontFamily::Proportional, egui::FontFamily::Monospace]
            .into_iter()
            .map(|family| egui::InsertFontFamily {
                family,
                priority: egui::FontPriority::Lowest,
            })
            .collect();
        Some(egui::FontInsert::new(
            &format!("cjk:{}#{index}", path.display()),
            egui::FontData::from_blob(blob, *index),
            families,
        ))
    }
}

/// Install the CJK provider for the app's language. Called at start and on every language change: changing the
/// providers rebuilds egui's fonts, so Han characters pick up the new reader's face. Font bytes stay cached.
pub(crate) fn apply(ctx: &egui::Context, lang: Lang) {
    ctx.set_font_providers(vec![Arc::new(CjkFonts { reader: lang })]);
}

/// The languages the selector must show disabled on this computer.
pub(crate) fn unavailable() -> Vec<Lang> {
    Lang::ALL.into_iter().filter(|l| !can_show(*l)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn characters_pick_the_script_that_has_them() {
        assert_eq!(Script::for_char('한', Lang::En), &[Script::Korean]);
        assert_eq!(Script::for_char('の', Lang::ZhCn)[0], Script::Japanese);
        assert_eq!(Script::for_char('语', Lang::En)[0], Script::Chinese);
        assert_eq!(Script::for_char('語', Lang::Ja)[0], Script::Japanese);
        assert_eq!(Script::for_char('語', Lang::Ko)[0], Script::Korean);
        assert!(Script::for_char('é', Lang::Ja).is_empty());
        assert!(Script::for_char('Ж', Lang::Ja).is_empty());
        for script in Script::ALL {
            assert!(!script.candidates().is_empty());
        }
    }

    #[test]
    fn languages_without_cjk_text_never_need_a_font() {
        for lang in Lang::ALL {
            assert_eq!(Script::of(lang).is_some(), lang.needs_cjk(), "{lang:?}");
            if !lang.needs_cjk() {
                assert!(can_show(lang));
            }
        }
    }

    #[cfg(not(windows))]
    #[test]
    fn fontconfig_answers_count_only_when_they_cover_the_language() {
        let noto = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc\t1\taa|ja|ko|zh-cn|zh-tw";
        assert_eq!(
            parse_fc_match(noto, Script::Korean),
            Some((PathBuf::from("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"), 1))
        );
        // fc-match falls back to any font; DejaVu has no Japanese.
        assert_eq!(
            parse_fc_match("/usr/share/fonts/TTF/DejaVuSans.ttf\t0\taa|en|ru", Script::Japanese),
            None
        );
        assert_eq!(parse_fc_match("", Script::Chinese), None);
        assert_eq!(parse_fc_match("relative.pcf\t0\tja", Script::Japanese), None);
    }

    #[test]
    fn the_provider_ignores_text_that_is_not_cjk() {
        use egui::FontProvider as _;
        let provider = CjkFonts { reader: Lang::En };
        let family = egui::FontFamily::Proportional;
        for cluster in ["a", "Ж", "ł", "→"] {
            assert!(
                provider
                    .font_for(&egui::FallbackRequest {
                        cluster,
                        family: &family
                    })
                    .is_none()
            );
        }
    }

    /// On a computer with a CJK font, the selector's native names come out as real glyphs, not boxes.
    #[test]
    fn cjk_names_render_with_a_system_font_when_there_is_one() {
        if !Lang::ALL.iter().all(|l| can_show(*l)) {
            eprintln!("no CJK system font here; nothing to draw");
            return;
        }
        let ctx = egui::Context::default();
        apply(&ctx, Lang::En);
        ctx.set_missing_glyph_policy(egui::MissingGlyphPolicy::Panic);
        for _ in 0..2 {
            let mut output = ctx.run_ui(Default::default(), |ui| {
                for lang in Lang::ALL {
                    ui.label(lang.native_name());
                }
            });
            output.textures_delta.clear();
        }
    }
}
