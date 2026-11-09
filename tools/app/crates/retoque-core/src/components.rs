//! Component payloads share the normal v1 receipt and its crash journal. The small INI index is itself
//! receipt-owned, so older full uninstallers can remove a component without understanding its identity.
use super::*;
use std::collections::BTreeSet;

pub(super) const INDEX: &str = ".retoque/components.txt";
const SECTION: &str = "DlssgSm86";

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 128 && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}
fn valid_path(path: &str) -> bool {
    safe_rel(path)
        && !is_model(path)
        && (path == "version.dll" || path == "dlssg_sm86.ini" || path.starts_with("dlssg_sm86/"))
}
fn paths(text: &str) -> Result<Vec<String>, Error> {
    let result: Vec<String> = text.split('|').map(String::from).collect();
    let unique: BTreeSet<_> = result.iter().map(|p| p.to_ascii_lowercase()).collect();
    if result.iter().any(|p| !valid_path(p)) || unique.len() != result.len() {
        return Err(usage_error("invalid component paths"));
    }
    Ok(result)
}
fn index(game: &Path, receipt: &Receipt) -> Result<Ini, Error> {
    check_parents(game, INDEX)?;
    let path = game.join(INDEX);
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
    let loader = game.join("version.dll");
    let config = game.join("dlssg_sm86.ini");
    plan.execution.snapshots.insert(loader.clone(), fingerprint(&loader)?);
    plan.execution.snapshots.insert(config.clone(), fingerprint(&config)?);
    let legacy = regular(&loader) && (regular(&config) || sm86(&loader)) && !marker(&loader, MARK_OPTI);
    if marker(&loader, MARK_OPTI) {
        plan.refusals.push(Refusal::Usage(
            "version.dll is Retoque's own proxy; reinstall Retoque with a different proxy before adding sm86".into(),
        ));
    }
    for name in PROXY_NAMES.into_iter().chain(["dinput8.dll"]) {
        if name == "version.dll" {
            continue;
        }
        // Include missing names and the directory itself so adding a second loader also stales a plan.
        observe(&mut plan, &game.join(name))?;
        for variant in variants(&game, name) {
            let path = game.join(&variant);
            observe(&mut plan, &path)?;
            if sm86(&path) {
                plan.refusals.push(Refusal::Usage(format!(
                    "{variant} is another sm86 loader; remove it before installing version.dll"
                )));
            }
        }
    }
    let backup = backup_dir(&game);
    for file in &files {
        check_parents(&game, file)?;
        exact_case(&game, file)?;
        check_parents(&source, file)?;
        let from = source.join(file);
        let to = game.join(file);
        if !regular(&from) {
            return Err(usage_error(format!("component source is not a regular file: {file}")));
        }
        if exists(&to) {
            if !regular(&to) {
                plan.refusals.push(foreign(file));
                continue;
            }
            let sha = hash(&to)?;
            let owned = owned(&receipt, file, &sha);
            if !owned && !legacy {
                plan.refusals.push(foreign(file));
                continue;
            }
            if !owned {
                plan.steps.push(Step::Backup {
                    path: file.clone(),
                    copy: format!("{backup}/{file}"),
                });
            }
            if hash(&from)? == sha {
                plan.steps.push(Step::Keep {
                    path: file.clone(),
                    why: "identical component file".into(),
                });
                continue;
            }
        }
        plan.steps.push(Step::Copy { from, to: file.clone() });
    }
    // Register first so interruption during payload copies is recoverable by component uninstall too.
    let mut metadata = Vec::new();
    for (key, value) in [
        ("Kind", "dlssg-sm86".to_string()),
        ("Paths", files.join("|")),
        ("Id", component.id.clone()),
    ] {
        let old = ini.get(SECTION, key).map(String::from);
        if old.as_deref() != Some(&value) {
            metadata.push(Step::SetIni {
                file: INDEX.into(),
                section: SECTION.into(),
                key: key.into(),
                old,
                value: value.clone(),
            });
            ini.set(SECTION, key, &value);
        }
    }
    metadata.append(&mut plan.steps);
    plan.steps = metadata;
    // Even unchanged index bytes must be guarded while installing an update.
    plan.execution
        .snapshots
        .insert(game.join(INDEX), fingerprint(&game.join(INDEX))?);
    plan.notes.push("Install user-imported dlssg-sm86; record its identity and files in .retoque/components.txt. Existing legacy files are backed up. No runtimes are downloaded.".into());
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
