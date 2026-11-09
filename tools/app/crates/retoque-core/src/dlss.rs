//! DLSS replacement is an ordinary v1 Backup/Copy install. No path may escape the discovered game root.
use super::*;
use std::collections::BTreeSet;
pub(crate) const INDEX: &str = ".retoque/dlss.txt";
const MARK: &[u8] = b"Retoque DLSS receipt v1\n";
pub(crate) fn dll(path: &str) -> bool {
    safe_rel(path)
        && path.rsplit('/').next().is_some_and(|n| {
            ["nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"]
                .iter()
                .any(|v| n.eq_ignore_ascii_case(v))
        })
        && !path.split('/').any(|p| {
            [".retoque", "dlssg_sm86", "OptiScaler", "DlssNrModels"]
                .iter()
                .any(|v| p.eq_ignore_ascii_case(v))
        })
}
fn own_receipt(root: &Path, r: &Receipt) -> Result<bool, Error> {
    let marker = root.join(INDEX);
    let expected = format!("{:x}", Sha256::digest(MARK));
    let marker_ok = if regular(&marker) {
        read(&marker)? == MARK
    } else {
        !exists(&marker) && r.status != ReceiptStatus::Complete
    };
    Ok(marker_ok
        && owned(r, INDEX, &expected)
        && r.records.iter().all(|r| match r {
            Record::File { path, .. } => path == INDEX || dll(path),
            Record::Backup { path, .. } => dll(path),
            _ => false,
        }))
}
pub(crate) fn root_state(root: &Path) -> Result<Option<ReceiptStatus>, Error> {
    let root = canonical(root)?;
    let Some(r) = load_receipt(&root)? else { return Ok(None) };
    if !own_receipt(&root, &r)? {
        return Err(usage_error("DLSS root receipt belongs to another installation"));
    }
    Ok(Some(r.status))
}
pub(crate) fn plan(root: &Path, target: &Path, source: &Path, files: &[String]) -> Result<Plan, Error> {
    let root = canonical(root)?;
    let target = canonical(target)?;
    if !target.starts_with(&root) || files.is_empty() {
        return Err(usage_error("invalid DLSS game root or empty file list"));
    }
    let mut seen = BTreeSet::new();
    for file in files {
        if !dll(file) || !seen.insert(file.to_ascii_lowercase()) {
            return Err(usage_error("invalid or duplicate DLSS path"));
        }
        check_parents(&root, file)?;
        if !regular(&root.join(file)) || variants(&root, file) != [file.clone()] {
            return Err(usage_error("DLSS destination missing, linked or ambiguous"));
        }
    }
    let in_target = files.iter().all(|p| root.join(p).starts_with(&target));
    let destination = if in_target { &target } else { &root };
    let standalone = destination != &target;
    // A previous layout must be restored first; never let two receipts own the same game DLL.
    if root != target {
        let other = if standalone { &target } else { &root };
        if let Some(r) = load_receipt(other)?
            && (standalone
                && r.records
                    .iter()
                    .any(|r| matches!(r, Record::File{path,..} if dll(path)))
                || !standalone && own_receipt(other, &r)?)
        {
            return Err(usage_error("restore previous DLSS receipt before changing its root"));
        }
    }
    let before = load_receipt(destination)?;
    if standalone
        && let Some(r) = &before
        && !own_receipt(destination, r)?
    {
        return Err(usage_error("DLSS root receipt belongs to another installation"));
    }
    if standalone && before.is_none() && exists(&destination.join(INDEX)) {
        return Err(usage_error("DLSS root marker is not receipt-owned"));
    }
    let mut plan = new_plan(destination, Operation::Install)?;
    plan.execution.process_roots = vec![root.clone(), target.clone()];
    plan.execution.component_completion = Some(before.as_ref().map_or(ReceiptStatus::Complete, |r| r.status));
    let receipt = before.unwrap_or_else(empty_receipt);
    let source = canonical(source)?;
    // Marker is a receipt-owned regular file, not a new receipt syntax. Write it before DLLs.
    if standalone && !exists(&destination.join(INDEX)) {
        let marker = source.join("receipt-marker.txt");
        if !regular(&marker) || read(&marker)? != MARK {
            return Err(usage_error("invalid DLSS catalog marker"));
        }
        plan.steps.push(Step::Copy {
            from: marker,
            to: INDEX.into(),
        });
    }
    let backup = backup_dir(destination);
    for file in files {
        let path = root.join(file);
        let rel = path
            .strip_prefix(destination)
            .unwrap()
            .to_str()
            .ok_or_else(|| usage_error("non-Unicode DLSS path"))?
            .replace('\\', "/");
        let name = path.file_name().unwrap().to_str().unwrap().to_ascii_lowercase();
        check_parents(&source, &name)?;
        let from = source.join(name);
        if !regular(&from) {
            return Err(usage_error("DLSS catalog file missing or linked"));
        }
        let old = hash(&path)?;
        if hash(&from)? == old {
            continue;
        }
        if !owned(&receipt, &rel, &old) {
            if receipt
                .records
                .iter()
                .any(|r| matches!(r,Record::File{path,..}|Record::Backup{path,..} if path == &rel))
            {
                return Err(usage_error(
                    "DLSS file changed since previous update; restore or reconcile first",
                ));
            }
            plan.steps.push(Step::Backup {
                path: rel.clone(),
                copy: format!("{backup}/{rel}"),
            });
        }
        plan.steps.push(Step::Copy { from, to: rel });
    }
    seal(plan, &receipt)
}
pub(crate) fn restore(root: &Path, target: &Path, root_only: bool) -> Result<Vec<Plan>, Error> {
    let root = canonical(root)?;
    let target = canonical(target)?;
    if !target.starts_with(&root) {
        return Err(usage_error("target outside DLSS game root"));
    }
    let mut plans = Vec::new();
    if root != target
        && let Some(r) = load_receipt(&root)?
    {
        if !own_receipt(&root, &r)? {
            return Err(usage_error("DLSS root receipt belongs to another installation"));
        }
        let mut p = new_plan(&root, Operation::Uninstall)?;
        p.execution.process_roots = vec![root.clone(), target.clone()];
        let mut payload = r.clone();
        payload
            .records
            .retain(|r| !matches!(r, Record::File { path, .. } if path == INDEX));
        p = uninstall_steps(p, &payload)?;
        let kept = p
            .steps
            .iter()
            .enumerate()
            .any(|(i, s)| matches!(s, Step::Keep { .. }) && !p.execution.forget.contains_key(&i));
        if !kept && exists(&root.join(INDEX)) {
            p.steps.push(Step::Remove { path: INDEX.into() });
        } else if !exists(&root.join(INDEX)) {
            let i = p.steps.len();
            if let Some(record) = r
                .records
                .iter()
                .find(|r| matches!(r,Record::File{path,..} if path == INDEX))
            {
                p.execution.forget.insert(i, record.clone());
                p.steps.push(Step::Keep {
                    path: INDEX.into(),
                    why: "already gone".into(),
                });
            }
        }
        plans.push(seal(p, &r)?);
    }
    if !root_only && let Some(r) = load_receipt(&target)? {
        let mut p = new_plan(&target, Operation::Uninstall)?;
        p.execution.process_roots = vec![root, target];
        plans.push(uninstall_steps(p, &r)?);
    }
    Ok(plans)
}
