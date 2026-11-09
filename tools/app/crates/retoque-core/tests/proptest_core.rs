use proptest::prelude::*;
use retoque_core::ini::Ini;
use retoque_core::receipt::{Receipt, ReceiptStatus, Record, safe_rel};

fn ident_strategy() -> impl Strategy<Value = String> {
    "[A-Za-z][A-Za-z0-9_]{0,15}"
}

fn value_strategy() -> impl Strategy<Value = String> {
    "[A-Za-z0-9_./:-]{1,24}"
}

fn safe_rel_strategy() -> impl Strategy<Value = String> {
    prop::collection::vec("[A-Za-z0-9_-]{1,12}(\\.[A-Za-z0-9]{1,4})?", 1..=4).prop_map(|parts| parts.join("/"))
}

fn sha_strategy() -> impl Strategy<Value = String> {
    "[0-9a-f]{8,64}"
}

fn canonical_receipt_strategy() -> impl Strategy<Value = Receipt> {
    let status = prop_oneof![
        Just(ReceiptStatus::Complete),
        Just(ReceiptStatus::Partial),
        Just(ReceiptStatus::UninstallIncomplete),
    ];
    let updated = "[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z";
    let proxies = prop::collection::btree_set(safe_rel_strategy(), 0..=3);
    let files = prop::collection::btree_map(safe_rel_strategy(), sha_strategy(), 0..=8);
    let backups = prop::collection::vec((sha_strategy(), safe_rel_strategy(), safe_rel_strategy()), 0..=4);
    let renames = prop::collection::vec((sha_strategy(), safe_rel_strategy(), safe_rel_strategy()), 0..=4);

    (status, updated, proxies, files, backups, renames).prop_map(
        |(status, updated, proxies, files, backups, renames)| {
            let mut records = Vec::new();
            for name in proxies {
                records.push(Record::Proxy { name });
            }
            for (path, sha256) in files {
                records.push(Record::File { sha256, path });
            }
            for (sha256, path, copy) in backups {
                records.push(Record::Backup { sha256, path, copy });
            }
            for (sha256, from, to) in renames {
                records.push(Record::Rename { sha256, from, to });
            }
            Receipt {
                status,
                updated,
                records,
            }
        },
    )
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    #[test]
    fn prop_ini_arbitrary_bytes_roundtrip_and_idempotence(
        raw in prop::collection::vec(any::<u8>(), 0..=512),
        section in ident_strategy(),
        key in ident_strategy(),
        val in value_strategy(),
    ) {
        let mut ini = Ini::parse(&raw);
        prop_assert_eq!(ini.to_bytes(), raw);

        let _ = ini.set(&section, &key, &val);
        prop_assert_eq!(ini.get(&section, &key), Some(val.as_str()));

        let after_first = ini.to_bytes();
        prop_assert_eq!(Ini::parse(&after_first).to_bytes(), after_first.as_slice());

        // Second identical set must be a no-op returning false and preserving exact bytes.
        prop_assert!(!ini.set(&section, &key, &val));
        prop_assert_eq!(ini.to_bytes(), after_first);
    }

    #[test]
    fn prop_ini_preserves_bom_crlf_invalid_utf8_comments_and_other_sections(
        has_bom in any::<bool>(),
        use_crlf in any::<bool>(),
        target_exists in any::<bool>(),
        key_exists in any::<bool>(),
        bad_utf8_byte in 0x80u8..=0xffu8,
        other_sec in "Other[A-Za-z0-9]{1,8}",
        other_key in "other_[a-z]{1,8}",
        other_val in value_strategy(),
        target_sec in "Target[A-Za-z0-9]{1,8}",
        target_key in "target_[a-z]{1,8}",
        initial_val in value_strategy(),
        new_val in value_strategy(),
    ) {
        let eol: &[u8] = if use_crlf { b"\r\n" } else { b"\n" };
        let mut raw = Vec::new();
        if has_bom {
            raw.extend_from_slice(b"\xef\xbb\xbf");
        }
        // Comment line with invalid UTF-8 bytes
        let mut comment_line = vec![b';', b' ', bad_utf8_byte, b' ', b'k', b'=', b'v'];
        comment_line.extend_from_slice(eol);
        raw.extend_from_slice(&comment_line);

        // Hash comment line
        let mut hash_comment = b"# untouched comment".to_vec();
        hash_comment.extend_from_slice(eol);
        raw.extend_from_slice(&hash_comment);

        // Other section
        raw.extend_from_slice(format!("[{other_sec}]").as_bytes());
        raw.extend_from_slice(eol);
        raw.extend_from_slice(format!("{other_key}={other_val}").as_bytes());
        raw.extend_from_slice(eol);

        if target_exists {
            raw.extend_from_slice(format!("[ {target_sec} ]").as_bytes());
            raw.extend_from_slice(eol);
            if key_exists {
                raw.extend_from_slice(format!("  {} = {initial_val}", target_key.to_ascii_uppercase()).as_bytes());
                raw.extend_from_slice(eol);
                raw.extend_from_slice(format!("{target_key}={initial_val}").as_bytes());
                raw.extend_from_slice(eol);
            }
        }

        let mut ini = Ini::parse(&raw);
        prop_assert_eq!(ini.to_bytes(), raw);
        prop_assert_eq!(ini.get(&other_sec, &other_key), Some(other_val.as_str()));

        let _ = ini.set(&target_sec.to_ascii_lowercase(), &target_key.to_ascii_uppercase(), &new_val);
        prop_assert_eq!(ini.get(&target_sec, &target_key), Some(new_val.as_str()));
        prop_assert_eq!(ini.get(&other_sec, &other_key), Some(other_val.as_str()));

        let out = ini.to_bytes();
        if has_bom {
            prop_assert!(out.starts_with(b"\xef\xbb\xbf"));
        }
        prop_assert!(out.windows(comment_line.len()).any(|w| w == comment_line.as_slice()));
        prop_assert!(out.windows(hash_comment.len()).any(|w| w == hash_comment.as_slice()));

        prop_assert!(!ini.set(&target_sec, &target_key, &new_val));
        prop_assert_eq!(ini.to_bytes(), out);
    }

    #[test]
    fn prop_receipt_roundtrip(receipt in canonical_receipt_strategy()) {
        let text = receipt.to_text();
        let parsed = Receipt::parse(&text).expect("valid receipt must parse");
        prop_assert_eq!(&parsed, &receipt);
        prop_assert_eq!(&parsed.to_text(), &text);

        // CRLF variant also parses identically.
        let crlf = text.replace('\n', "\r\n");
        let parsed_crlf = Receipt::parse(&crlf).expect("CRLF receipt must parse");
        prop_assert_eq!(parsed_crlf, receipt);
    }

    #[test]
    fn prop_safe_rel_accepts_valid_and_rejects_traversal_drive_control_trailing(
        valid in safe_rel_strategy(),
        prefix in "[a-z]{1,6}",
        suffix in "[a-z]{1,6}",
        ctrl in 0u8..32u8,
        drive in "[A-Za-z]",
    ) {
        prop_assert!(safe_rel(&valid));

        let bad_cases = [
            format!("{valid}/../{suffix}"),
            format!("../{valid}"),
            format!("{valid}/.."),
            format!("{valid}/./{suffix}"),
            format!("/{valid}"),
            format!("{valid}/"),
            format!("{valid}//{suffix}"),
            format!("{drive}:{valid}"),
            format!("{drive}:\\{valid}"),
            format!("{prefix}\\{suffix}"),
            format!("{prefix} /{suffix}"),
            format!("{prefix}./{suffix}"),
            format!("{valid} "),
            format!("{valid}."),
            format!("{prefix}{}{suffix}", ctrl as char),
        ];

        for bad in &bad_cases {
            prop_assert!(!safe_rel(bad), "safe_rel accepted unsafe path: {:?}", bad);
            let receipt_txt = format!("version\t1\nstatus\tcomplete\nupdated\tnow\nfile\tabc\t{bad}\n");
            prop_assert!(Receipt::parse(&receipt_txt).is_err(), "Receipt::parse accepted unsafe path: {:?}", bad);
        }
    }
}
