use std::fs::{self, File};
use std::io::{self, Read};

use proptest::prelude::*;
use retoque_core::ini::Ini;
use retoque_data::logs::bundle;
use retoque_data::settings::{self, Kind, Setting};

struct ChunkedReader<'a> {
    data: &'a [u8],
    chunk_size: usize,
}

impl Read for ChunkedReader<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.data.is_empty() || buf.is_empty() {
            return Ok(0);
        }
        let n = buf.len().min(self.chunk_size.max(1)).min(self.data.len());
        buf[..n].copy_from_slice(&self.data[..n]);
        self.data = &self.data[n..];
        Ok(n)
    }
}

fn word_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte >= 128 || matches!(byte, b'_' | b'-' | b'.')
}

fn component_end(byte: u8) -> bool {
    matches!(
        byte,
        b'/' | b'\\' | b'"' | b'\'' | b'\r' | b'\n' | b'\t' | b'<' | b'>' | b'{' | b'}'
    )
}

/// Pure streaming reference mirror of Redactor::copy to test arbitrary read chunk sizes 1..=65536
/// without modifying logs.rs (owned by app-smart), verified against `retoque_data::logs::bundle`.
fn redact_stream(mut input: impl Read, homes: &[String], users: &[String]) -> Vec<u8> {
    let mut replacements = Vec::new();
    for home in homes.iter().filter(|s| !s.is_empty() && s.len() > 1) {
        let slash = home.replace('\\', "/").trim_end_matches('/').to_owned();
        for spelling in [home.clone(), slash.clone(), slash.replace('/', "\\")] {
            replacements.push((spelling.as_bytes().to_vec(), b"~".as_slice()));
            replacements.push((spelling.replace('\\', "\\\\").into_bytes(), b"~".as_slice()));
            if slash.starts_with('/') {
                replacements.push((format!("Z:{spelling}").into_bytes(), b"~".as_slice()));
                replacements.push((
                    format!("Z:{spelling}").replace('\\', "\\\\").into_bytes(),
                    b"~".as_slice(),
                ));
            }
        }
    }
    for user in users.iter().filter(|s| !s.is_empty()) {
        replacements.push((user.as_bytes().to_vec(), b"<user>".as_slice()));
    }
    replacements.sort_by_key(|(s, _)| std::cmp::Reverse(s.len()));
    replacements.dedup();

    let mut markers = Vec::new();
    for separator in ["/", r"\/", r"\", r"\\", r"\\\\"] {
        for component in ["home", "Users", "userdata"] {
            markers.push((
                format!("{separator}{component}{separator}").into_bytes(),
                component == "userdata",
            ));
        }
    }
    markers.sort_by_key(|(marker, _)| std::cmp::Reverse(marker.len()));
    let overlap = replacements
        .iter()
        .map(|(s, _)| s.len() + 1)
        .chain(markers.iter().map(|(s, _)| s.len() + 1))
        .max()
        .unwrap_or(1);

    let mut output = Vec::new();
    let mut block = [0u8; 65536];
    let mut pending = Vec::new();
    let mut previous = None;
    let mut hiding: Option<bool> = None;
    loop {
        let count = input.read(&mut block).unwrap();
        pending.extend_from_slice(&block[..count]);
        let limit = if count == 0 {
            pending.len()
        } else {
            pending.len().saturating_sub(overlap)
        };
        let mut at = 0;
        let mut clean = Vec::with_capacity(limit);
        while at < limit {
            let byte = pending[at];
            if let Some(steam) = hiding {
                if !component_end(byte) && (!steam || byte.is_ascii_digit()) {
                    previous = Some(byte);
                    at += 1;
                    continue;
                }
                hiding = None;
            }
            if let Some((source, replacement)) = replacements.iter().find(|(source, replacement)| {
                let end = at + source.len();
                let matches = pending
                    .get(at..end)
                    .is_some_and(|candidate| candidate.eq_ignore_ascii_case(source));
                let after = pending.get(end).copied();
                matches
                    && if *replacement == b"<user>" {
                        previous.is_none_or(|b| !word_byte(b)) && after.is_none_or(|b| !word_byte(b))
                    } else {
                        after.is_none_or(component_end)
                    }
            }) {
                clean.extend_from_slice(replacement);
                at += source.len();
                previous = pending.get(at - 1).copied();
            } else if let Some((marker, steam)) = markers.iter().find(|(marker, steam)| {
                let end = at + marker.len();
                pending.get(at..end).is_some_and(|s| s.eq_ignore_ascii_case(marker))
                    && pending
                        .get(end)
                        .is_some_and(|b| !component_end(*b) && (!*steam || b.is_ascii_digit()))
            }) {
                clean.extend_from_slice(&pending[at..at + marker.len()]);
                clean.extend_from_slice(if *steam { b"<steam-id>" } else { b"<user>" });
                at += marker.len();
                previous = pending.get(at - 1).copied();
                hiding = Some(*steam);
            } else {
                clean.push(byte);
                previous = Some(byte);
                at += 1;
            }
        }
        output.extend_from_slice(&clean);
        pending.drain(..at);
        if count == 0 {
            return output;
        }
    }
}

fn valid_value_for_setting(setting: &Setting) -> BoxedStrategy<String> {
    match setting.kind {
        Kind::Bool => prop_oneof![
            Just("auto".to_string()),
            Just("true".to_string()),
            Just("false".to_string())
        ]
        .boxed(),
        Kind::Choice(choices) => prop::sample::select(choices).prop_map(|s| s.to_string()).boxed(),
        Kind::Float { min, max } => prop_oneof![
            Just("auto".to_string()),
            (0u32..=1000u32).prop_map(move |step| {
                let frac = step as f32 / 1000.0;
                let v = min + (max - min) * frac;
                let clamped = v.clamp(min, max);
                format!("{clamped:.3}")
            }),
        ]
        .boxed(),
        Kind::Int { min, max } => {
            prop_oneof![Just("auto".to_string()), (min..=max).prop_map(|n| n.to_string()),].boxed()
        }
        Kind::Text => prop_oneof![Just("auto".to_string()), "[A-Za-z0-9_./:\\\\-]{1,40}",].boxed(),
    }
}

fn setting_and_valid_value() -> impl Strategy<Value = (Setting, String)> {
    prop::sample::select(settings::all()).prop_flat_map(|s| {
        let strat = valid_value_for_setting(&s);
        (Just(s), strat)
    })
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(48))]

    #[test]
    fn prop_redactor_never_contains_private_id_and_is_chunk_size_independent(
        private_id in "privid_[a-z0-9]{6,14}",
        steam_id in "7656119[0-9]{10}",
        chunk_a in 1usize..=64,
        chunk_b in 65usize..=65536,
        pad_len in 0usize..=512,
        sep_idx in 0usize..5,
    ) {
        let sep = ["/", r"\/", r"\", r"\\", r"\\\\"][sep_idx];
        let pad = "x".repeat(pad_len);
        let payload = format!(
            "{pad} {sep}home{sep}{private_id}{sep}model.dll {sep}Users{sep}{private_id}{sep}OptiScaler.ini {sep}userdata{sep}{steam_id}{sep}config {private_id}"
        );

        let homes = vec![format!("/synthetic/home/{private_id}")]; // privacy: allow (synthetic test path)
        let users = vec![private_id.clone()];

        let out_a = redact_stream(
            ChunkedReader {
                data: payload.as_bytes(),
                chunk_size: chunk_a,
            },
            &homes,
            &users,
        );
        let out_b = redact_stream(
            ChunkedReader {
                data: payload.as_bytes(),
                chunk_size: chunk_b,
            },
            &homes,
            &users,
        );
        let out_full = redact_stream(
            ChunkedReader {
                data: payload.as_bytes(),
                chunk_size: 65536,
            },
            &homes,
            &users,
        );

        prop_assert_eq!(&out_a, &out_b);
        prop_assert_eq!(&out_a, &out_full);

        let redacted_str = String::from_utf8(out_a).unwrap();
        prop_assert!(!redacted_str.contains(&private_id), "leaked private_id in {redacted_str}");
        prop_assert!(!redacted_str.contains(&steam_id), "leaked steam_id in {redacted_str}");

        // Also verify through the public `retoque_data::logs::bundle` API on path markers.
        let path_only_payload = format!(
            "{pad} {sep}home{sep}{private_id}{sep}model.dll {sep}Users{sep}{private_id}{sep}OptiScaler.ini {sep}userdata{sep}{steam_id}{sep}config"
        );
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join("OptiScaler.log"), &path_only_payload).unwrap();
        let zip_path = bundle(&game, &path_only_payload, &temp.path().join("out")).unwrap();
        let mut archive = zip::ZipArchive::new(File::open(zip_path).unwrap()).unwrap();
        for idx in 0..archive.len() {
            let mut text = String::new();
            archive.by_index(idx).unwrap().read_to_string(&mut text).unwrap();
            prop_assert!(!text.contains(&private_id), "bundle leaked private_id in member {idx}: {text}");
            prop_assert!(!text.contains(&steam_id), "bundle leaked steam_id in member {idx}: {text}");
        }
    }

    #[test]
    fn prop_setting_accepts_values_survive_ini_roundtrip(
        (setting, val) in setting_and_valid_value(),
        leading_ws in prop_oneof![Just(""), Just(" "), Just("  ")],
        trailing_ws in prop_oneof![Just(""), Just(" "), Just("\t")],
    ) {
        let padded = format!("{leading_ws}{val}{trailing_ws}");
        prop_assert!(setting.accepts(&padded), "{} {} rejected {:?}", setting.section, setting.key, padded);

        let mut ini = Ini::parse(b"; header comment\r\n[Other]\r\nKeep=1\r\n");
        let _ = ini.set(setting.section, setting.key, padded.trim());
        let bytes = ini.to_bytes();
        let roundtrip = Ini::parse(&bytes);
        let got = roundtrip.get(setting.section, setting.key).expect("roundtrip value must exist");
        prop_assert_eq!(got, val.as_str());
        prop_assert!(setting.accepts(got));
    }
}
