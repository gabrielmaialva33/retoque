//! Component payloads share the normal v1 receipt and its crash journal. The small INI index is itself
//! receipt-owned, so older full uninstallers can remove a component without understanding its identity.
use super::*;
use std::collections::BTreeSet;

pub(super) const INDEX: &str = ".retoque/components.txt";
const SECTION: &str = "DlssgSm86";
const VS_VERSION_INFO_UTF16LE: [u8; 32] = [
    b'V', 0, b'S', 0, b'_', 0, b'V', 0, b'E', 0, b'R', 0, b'S', 0, b'I', 0, b'O', 0, b'N', 0, b'_', 0, b'I', 0, b'N',
    0, b'F', 0, b'O', 0, 0, 0,
];

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 128 && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}
fn valid_path(path: &str) -> bool {
    safe_rel(path)
        && !is_model(path)
        && (path == "version.dll" || path == "dlssg_sm86.ini" || path.starts_with("dlssg_sm86/"))
}
fn paths(text: &str) -> Result<Vec<String>, Error> {
    let result: Vec<String> = text.split('|').map(|p| p.replace('\\', "/")).collect();
    let unique: BTreeSet<_> = result.iter().map(|p| p.to_ascii_lowercase()).collect();
    if result.iter().any(|p| !valid_path(p)) || unique.len() != result.len() {
        return Err(usage_error("invalid component paths"));
    }
    Ok(result)
}
fn join_rel(base: &Path, rel: &str) -> PathBuf {
    let mut out = base.to_path_buf();
    for part in rel.split(['/', '\\']).filter(|p| !p.is_empty()) {
        out.push(part);
    }
    out
}
fn index(game: &Path, receipt: &Receipt) -> Result<Ini, Error> {
    check_parents(game, INDEX)?;
    let path = join_rel(game, INDEX);
    if !exists(&path) {
        return Ok(Ini::default());
    }
    if !regular(&path) || !owned(receipt, INDEX, &hash(&path)?) {
        return Err(Error::Refused(vec![foreign(
            "component index is not an unchanged receipt-owned file",
        )]));
    }
    let ini = Ini::parse(&read(&path)?);
    if let Some(id) = ini.get(SECTION, "Id") {
        if !valid_id(id) || ini.get(SECTION, "Kind") != Some("dlssg-sm86") {
            return Err(usage_error("invalid component index"));
        }
        paths(ini.get(SECTION, "Paths").unwrap_or(""))?;
    }
    Ok(ini)
}
fn utf16le_key_has_optiscaler(block: &[u8], key: &str) -> bool {
    let key_bytes: Vec<u8> = key.encode_utf16().chain([0]).flat_map(u16::to_le_bytes).collect();
    for (pos, window) in block.windows(key_bytes.len()).enumerate() {
        if window != key_bytes.as_slice() {
            continue;
        }
        let mut v = pos + key_bytes.len();
        for _ in 0..2 {
            if block.get(v..v + 2) == Some(&[0, 0]) {
                v += 2;
            } else {
                break;
            }
        }
        let mut value = String::new();
        if let Some(rest) = block.get(v..) {
            for &chunk in rest.as_chunks::<2>().0.iter().take(128) {
                let code = u16::from_le_bytes(chunk);
                if code == 0 {
                    break;
                }
                if let Some(ch) = char::from_u32(u32::from(code)) {
                    value.push(ch);
                }
            }
        }
        if value.to_ascii_lowercase().contains("optiscaler") {
            return true;
        }
    }
    false
}
fn pe_optiscaler(path: &Path) -> bool {
    let Ok(bytes) = read(path) else {
        return false;
    };
    if !bytes.starts_with(b"MZ") {
        return false;
    }
    if bytes
        .windows(VS_VERSION_INFO_UTF16LE.len())
        .any(|w| w == VS_VERSION_INFO_UTF16LE)
    {
        return utf16le_key_has_optiscaler(&bytes, "FileDescription")
            || utf16le_key_has_optiscaler(&bytes, "ProductName");
    }
    false
}
fn is_optiscaler(path: &Path, rel: &str, receipt: &Receipt) -> bool {
    regular(path)
        && (marker(path, MARK_OPTI)
            || pe_optiscaler(path)
            || receipt
                .records
                .iter()
                .any(|r| matches!(r, Record::Proxy { name } if name.eq_ignore_ascii_case(rel)))
            || (!rel.eq_ignore_ascii_case("version.dll")
                && hash(path).is_ok_and(|sha| {
                    receipt.records.iter().any(
                    |r| matches!(r, Record::File { path: p, sha256 } if p.eq_ignore_ascii_case(rel) && sha256 == &sha),
                )
                })))
}
fn sm86(path: &Path) -> bool {
    regular(path)
        && [b"dlssg_sm86".as_slice(), b"dlssg_for_sm86", b"DLSSG SM86"]
            .iter()
            .any(|needle| marker(path, needle))
}
fn observe(plan: &mut Plan, path: &Path) -> Result<(), Error> {
    plan.execution.snapshots.insert(path.to_owned(), fingerprint(path)?);
    plan.execution
        .observed_files
        .insert(path.to_owned(), observed_file(path)?);
    Ok(())
}
// Windows resolves every component without case; refuse ambiguous or differently cased destinations
// on Linux as well instead of creating two parallel component trees.
fn exact_case(game: &Path, rel: &str) -> Result<(), Error> {
    let mut prefix = String::new();
    for part in rel.split('/') {
        if !prefix.is_empty() {
            prefix.push('/');
        }
        prefix.push_str(part);
        let names = variants(game, &prefix);
        if names.len() > 1 || names.first().is_some_and(|n| n != &prefix) {
            return Err(Error::Refused(vec![foreign(format!(
                "conflicting component path casing: {prefix}"
            ))]));
        }
    }
    Ok(())
}

pub(crate) fn installed(game: &Path) -> Result<Vec<String>, Error> {
    let game = canonical(game)?;
    let receipt = load_receipt(&game)?.unwrap_or_else(empty_receipt);
    Ok(index(&game, &receipt)?
        .get(SECTION, "Id")
        .map(String::from)
        .into_iter()
        .collect())
}

pub(crate) fn plan_install(game: &Path, component: &ComponentFiles) -> Result<Plan, Error> {
    if !valid_id(&component.id) {
        return Err(usage_error("invalid component id"));
    }
    let mut files = paths(&component.files.join("|"))?;
    files.sort();
    if !files.iter().any(|p| p == "version.dll")
        || !files.iter().any(|p| p == "dlssg_sm86.ini")
        || !files.iter().any(|p| p.starts_with("dlssg_sm86/"))
    {
        return Err(usage_error(
            "sm86 needs version.dll, dlssg_sm86.ini and dlssg_sm86 payload files",
        ));
    }
    let source = canonical(&component.source_root)?;
    let mut plan = new_plan(game, Operation::Install)?;
    let game = plan.game_dir.clone();
    let before = load_receipt(&game)?;
    plan.execution.component_completion = Some(before.as_ref().map_or(ReceiptStatus::Complete, |r| r.status));
    let receipt = before.unwrap_or_else(empty_receipt);
    let mut ini = index(&game, &receipt)?;
    if let Some(previous) = ini.get(SECTION, "Paths") {
        let mut previous = paths(previous)?;
        previous.sort();
        if previous != files {
            return Err(usage_error(
                "uninstall the existing component before changing its file layout",
            ));
        }
    }
    let loader = join_rel(&game, "version.dll");
    let config = join_rel(&game, "dlssg_sm86.ini");
    plan.execution.snapshots.insert(loader.clone(), fingerprint(&loader)?);
    plan.execution.snapshots.insert(config.clone(), fingerprint(&config)?);
    let own_version_proxy = is_optiscaler(&loader, "version.dll", &receipt);
    let legacy = regular(&loader) && (regular(&config) || sm86(&loader)) && !own_version_proxy;
    let adopting =
        (regular(&loader) && hash(&loader).ok() == hash(&join_rel(&source, "version.dll")).ok() && !own_version_proxy)
            || join_rel(&game, "dlssg_sm86").is_dir()
            || ini.get(SECTION, "Id").is_some();
    if own_version_proxy {
        plan.refusals.push(Refusal::Usage(
            "version.dll is Retoque's own proxy; reinstall Retoque with a different proxy before adding sm86".into(),
        ));
    }
    for name in PROXY_NAMES.into_iter().chain(["dinput8.dll"]) {
        if name == "version.dll" {
            continue;
        }
        // Include missing names and the directory itself so adding a second loader also stales a plan.
        observe(&mut plan, &join_rel(&game, name))?;
        for variant in variants(&game, name) {
            let path = join_rel(&game, &variant);
            observe(&mut plan, &path)?;
            if !is_optiscaler(&path, &variant, &receipt) && sm86(&path) {
                plan.refusals.push(Refusal::Usage(format!(
                    "{variant} is another sm86 loader; remove it before installing version.dll"
                )));
            }
        }
    }
    // Register first so interruption during payload copies is recoverable by component uninstall too.
    for (key, value) in [
        ("Kind", "dlssg-sm86".to_string()),
        ("Paths", files.join("|")),
        ("Id", component.id.clone()),
    ] {
        let old = ini.get(SECTION, key).map(String::from);
        if old.as_deref() != Some(&value) {
            plan.steps.push(Step::SetIni {
                file: INDEX.into(),
                section: SECTION.into(),
                key: key.into(),
                old,
                value: value.clone(),
            });
            ini.set(SECTION, key, &value);
        }
    }
    let backup = backup_dir(&game);
    let monolithic = legacy && fs::metadata(&loader).is_ok_and(|m| m.len() >= 20 * 1024 * 1024);
    let mut removed_legacy = BTreeSet::new();
    if monolithic {
        // 0.4.x requires a clean proxy/INI transition, not overlaying 0.3.x. Backups stay in
        // receipt v1 so both the app and existing uninstall scripts can restore the prior version.
        for file in ["version.dll", "dlssg_sm86.ini"] {
            check_parents(&game, file)?;
            exact_case(&game, file)?;
            if regular(&join_rel(&game, file)) {
                plan.steps.push(Step::Backup {
                    path: file.into(),
                    copy: format!("{backup}/{file}"),
                });
                plan.steps.push(Step::Remove { path: file.into() });
                removed_legacy.insert(file.to_owned());
            }
        }
    }
    for file in &files {
        check_parents(&game, file)?;
        exact_case(&game, file)?;
        check_parents(&source, file)?;
        let from = join_rel(&source, file);
        let to = join_rel(&game, file);
        if !regular(&from) {
            return Err(usage_error(format!("component source is not a regular file: {file}")));
        }
        if exists(&to) && !removed_legacy.contains(file) {
            if !regular(&to) {
                plan.refusals.push(foreign(file));
                continue;
            }
            let sha = hash(&to)?;
            let src_sha = hash(&from)?;
            let owned = owned(&receipt, file, &sha);
            if file == "dlssg_sm86.ini" && (adopting || owned) {
                if !owned {
                    let index = plan.steps.len();
                    let record = receipt
                        .records
                        .iter()
                        .find(|r| matches!(r, Record::File { path, .. } if path == file))
                        .cloned()
                        .unwrap_or_else(|| Record::File {
                            path: file.clone(),
                            sha256: String::new(),
                        });
                    plan.execution.forget.insert(index, record);
                    plan.steps.push(Step::Keep {
                        path: file.clone(),
                        why: "your own settings file, kept as it is".into(),
                    });
                    continue;
                }
                if src_sha == sha {
                    plan.steps.push(Step::Keep {
                        path: file.clone(),
                        why: "identical component file".into(),
                    });
                    continue;
                }
            } else if src_sha == sha {
                plan.steps.push(Step::Keep {
                    path: file.clone(),
                    why: "identical component file".into(),
                });
                continue;
            } else if !owned {
                if legacy && matches!(file.as_str(), "version.dll" | "dlssg_sm86.ini") {
                    plan.steps.push(Step::Backup {
                        path: file.clone(),
                        copy: format!("{backup}/{file}"),
                    });
                } else {
                    plan.refusals.push(foreign(file));
                    continue;
                }
            }
        }
        plan.steps.push(Step::Copy { from, to: file.clone() });
    }
    // Even unchanged index bytes must be guarded while installing an update.
    let index_path = join_rel(&game, INDEX);
    plan.execution
        .snapshots
        .insert(index_path.clone(), fingerprint(&index_path)?);
    if monolithic {
        plan.notes.push(
            "Upgrade sm86 0.3.x: back up and remove the old proxy and INI before copying the complete 0.4.x runtime."
                .into(),
        );
    }
    plan.notes.push("Install catalogued dlssg-sm86; record its identity and files in .retoque/components.txt. The game installer makes no network requests.".into());
    seal(plan, &receipt)
}

pub(crate) fn plan_uninstall(game: &Path, id: &str) -> Result<Plan, Error> {
    let mut plan = new_plan(game, Operation::Uninstall)?;
    plan.execution.component_only = true;
    let receipt = load_receipt(&plan.game_dir)?.ok_or_else(|| usage_error("no component receipt"))?;
    plan.execution.component_completion = Some(receipt.status);
    let ini = index(&plan.game_dir, &receipt)?;
    if ini.get(SECTION, "Id") != Some(id) {
        return Err(usage_error("component id is not installed"));
    }
    let files = paths(ini.get(SECTION, "Paths").unwrap_or(""))?;
    let mut selected = receipt.clone();
    selected.records.retain(|r| match r {
        Record::File { path, .. } | Record::Backup { path, .. } => files.contains(path),
        _ => false,
    });
    plan = uninstall_steps(plan, &selected)?;
    for (i, step) in plan.steps.iter().enumerate() {
        if matches!(step, Step::Keep { path, .. } if path == "dlssg_sm86.ini")
            && let Some(record) = selected
                .records
                .iter()
                .find(|r| matches!(r, Record::File { path, .. } if path == "dlssg_sm86.ini"))
        {
            plan.execution.forget.insert(i, record.clone());
        }
    }
    let kept = plan
        .steps
        .iter()
        .enumerate()
        .any(|(i, step)| matches!(step, Step::Keep { .. }) && !plan.execution.forget.contains_key(&i));
    if !kept {
        plan.steps.push(Step::Remove { path: INDEX.into() });
    } else {
        plan.notes.push(
            "Modified or unrestorable component files remain; its index and receipt are retained for recovery.".into(),
        );
    }
    plan.notes.push(format!(
        "Remove only component {id}; other Retoque files stay installed."
    ));
    seal(plan, &receipt)
}
