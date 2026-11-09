//! App-owned, bounded settings history. The receipt and game folder never store UI history.
use crate::{
    error::AppError,
    i18n::{Lang, t},
    model::{self, Edit, Outcome, PlanKind},
};
use retoque_core::{Plan, Step};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
    sync::mpsc::Sender,
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) struct Change {
    pub section: String,
    pub key: String,
    pub before: Option<String>,
    pub after: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Applied {
    pub time_unix: u64,
    pub changes: Vec<Change>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    exact: Vec<retoque_core::ini::IniUndo>,
}
#[derive(Default, Serialize, Deserialize)]
struct History {
    entries: Vec<Applied>,
}

pub(crate) fn default_root() -> Option<PathBuf> {
    dirs::data_local_dir().map(|p| p.join("retoque").join("settings-history"))
}
fn file(root: &Path, target: &Path) -> io::Result<PathBuf> {
    let target = fs::canonicalize(target)?;
    Ok(root.join(format!(
        "{:x}.json",
        Sha256::digest(target.as_os_str().as_encoded_bytes())
    )))
}
fn load(path: &Path) -> io::Result<History> {
    let f = match fs::File::open(path) {
        Ok(f) => f,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(History::default()),
        Err(e) => return Err(e),
    };
    let mut bytes = Vec::new();
    f.take(1024 * 1024 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > 1024 * 1024 {
        return Err(io::Error::other("settings history exceeds size limit"));
    }
    let history: History = serde_json::from_slice(&bytes)?;
    if history.entries.len() > 20 {
        return Err(io::Error::other("settings history exceeds entry limit"));
    }
    Ok(history)
}
pub(crate) fn capture(plan: &Plan) -> Applied {
    Applied {
        time_unix: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs(),
        exact: plan.settings_undo().to_vec(),
        changes: plan
            .steps()
            .iter()
            .filter_map(|s| match s {
                Step::SetIni {
                    section,
                    key,
                    old,
                    value,
                    ..
                } => Some(Change {
                    section: section.clone(),
                    key: key.clone(),
                    before: old.clone(),
                    after: Some(value.clone()),
                }),
                Step::UnsetIni { section, key, old, .. } => Some(Change {
                    section: section.clone(),
                    key: key.clone(),
                    before: old.clone(),
                    after: None,
                }),
                _ => None,
            })
            .collect(),
    }
}
pub(crate) fn record(root: &Path, target: &Path, mut entry: Applied) -> io::Result<()> {
    if entry.changes.is_empty() {
        return Ok(());
    }
    // Persist exact provenance only after the successful apply still matches the planned final hash.
    // Never attach an externally changed post-apply snapshot to the planned inverse.
    if !entry.exact.is_empty() {
        let matches = retoque_core::read_ini(target)
            .map_err(io::Error::other)?
            .is_some_and(|(_, bytes)| entry.exact.last().is_some_and(|u| u.matches_after(&bytes)));
        if !matches {
            entry.exact.clear();
        }
    }
    entry.time_unix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    fs::create_dir_all(root)?;
    let path = file(root, target)?;
    let mut history = load(&path)?;
    history.entries.push(entry);
    if history.entries.len() > 20 {
        history.entries.remove(0);
    }
    let bytes = serde_json::to_vec(&history)?;
    if bytes.len() > 1024 * 1024 {
        return Err(io::Error::other("settings history exceeds size limit"));
    }
    let mut temp = tempfile::NamedTempFile::new_in(root)?;
    temp.write_all(&bytes)?;
    temp.flush()?;
    temp.as_file().sync_all()?;
    temp.persist(&path).map_err(|e| e.error)?;
    Ok(())
}
/// A fresh sealed core plan binds the inverse to exactly the INI bytes checked here.
/// Missing original keys are removed. Exact provenance is optional for backward-compatible history.
pub(crate) fn undo_plan(root: &Path, target: &Path, lang: Lang) -> Result<Plan, AppError> {
    let path = file(root, target).map_err(AppError::from)?;
    let history = load(&path).map_err(|e| AppError::io(path, e))?;
    let entry = history
        .entries
        .last()
        .ok_or_else(|| AppError::from(t(lang, "undo.empty")))?;
    let edits = entry
        .changes
        .iter()
        .rev()
        .map(|c| (c.section.clone(), c.key.clone(), c.before.clone()))
        .collect::<Vec<_>>();
    let plan = retoque_core::plan_settings_undo(target, &edits, &entry.exact).map_err(AppError::from)?;
    if !plan.refusals().is_empty() {
        return Ok(plan);
    }
    // Inspect the old values *inside* the sealed plan, never an earlier UI snapshot/read.
    // Missing steps (already reverted or externally changed to the inverse) also refuse undo.
    if plan.steps().len() != entry.changes.len() {
        return Err(t(lang, "undo.conflict").into());
    }
    for (c, step) in entry.changes.iter().rev().zip(plan.steps()) {
        let matches = match step {
            Step::SetIni { section, key, old, .. } | Step::UnsetIni { section, key, old, .. } => {
                section == &c.section && key == &c.key && old == &c.after
            }
            _ => false,
        };
        if !matches {
            return Err(format!("{} [{}] {}", t(lang, "undo.conflict"), c.section, c.key).into());
        }
    }
    Ok(plan)
}
#[allow(clippy::too_many_arguments)]
pub(crate) fn apply_recorded(
    root: Option<&Path>,
    kind: PlanKind,
    plan: Plan,
    target: PathBuf,
    edits: Vec<Edit>,
    progress: &Sender<Outcome>,
    ctx: &egui::Context,
) -> Outcome {
    let record_entry = (kind == PlanKind::Settings).then(|| capture(&plan));
    let outcome = model::apply(kind, plan, target.clone(), edits, progress, ctx);
    if matches!(&outcome, Outcome::Applied { result: Ok(()), .. })
        && let Some(entry) = record_entry
        && !entry.changes.is_empty()
    {
        let result = root
            .ok_or_else(|| io::Error::other("application data directory unavailable"))
            .and_then(|root| record(root, &target, entry));
        if let Err(error) = result {
            let _ = progress.send(Outcome::HistoryWritten(Err(error.into())));
        }
    }
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn undo_roundtrip_conflicts_staleness_and_retention() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("game");
        let root = dir.path().join("data");
        fs::create_dir(&target).unwrap();
        let ini = target.join("OptiScaler.ini");
        fs::write(&ini, b"; preserve\r\n[DlssNr]\r\nEnabled=auto\r\n").unwrap();
        let plan = retoque_core::plan_settings(&target, &[("DlssNr".into(), "Enabled".into(), "true".into())]).unwrap();
        let entry = capture(&plan);
        let (tx, _) = std::sync::mpsc::channel();
        assert!(matches!(
            apply_recorded(
                Some(&root),
                PlanKind::Settings,
                plan,
                target.clone(),
                vec![],
                &tx,
                &egui::Context::default()
            ),
            Outcome::Applied { result: Ok(()), .. }
        ));
        assert!(!target.join(".retoque").exists());
        for lang in [Lang::En, Lang::Pt] {
            let inverse = undo_plan(&root, &target, lang).unwrap();
            assert!(
                matches!(&inverse.steps()[0], Step::SetIni {old: Some(v), value, ..} if v=="true" && value=="auto")
            );
            fs::write(&ini, "[DlssNr]\nEnabled=false\n").unwrap();
            assert!(
                undo_plan(&root, &target, lang)
                    .err()
                    .unwrap()
                    .to_string()
                    .contains(t(lang, "undo.conflict"))
            );
            assert!(retoque_core::apply(&inverse.into_verified().unwrap(), &mut |_| {}).is_err());
            fs::write(&ini, "[DlssNr]\nEnabled=true\n").unwrap();
        }
        for _ in 0..25 {
            record(&root, &target, entry.clone()).unwrap();
        }
        assert_eq!(load(&file(&root, &target).unwrap()).unwrap().entries.len(), 20);
        let plan = undo_plan(&root, &target, Lang::En).unwrap();
        retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
        assert_eq!(
            retoque_core::ini::Ini::parse(&fs::read(ini).unwrap()).get("DlssNr", "Enabled"),
            Some("auto")
        );
    }
    #[test]
    fn failed_apply_never_records_and_missing_before_is_removed() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("history");
        let ini = dir.path().join("OptiScaler.ini");
        fs::write(&ini, "[DlssNr]\n").unwrap();
        let plan = retoque_core::plan_settings(dir.path(), &[("DlssNr".into(), "Stage".into(), "1".into())]).unwrap();
        let entry = capture(&plan);
        assert_eq!(entry.changes[0].before, None);
        fs::write(&ini, "[DlssNr]\nStage=0\n").unwrap();
        let (tx, _) = std::sync::mpsc::channel();
        assert!(matches!(
            apply_recorded(
                Some(&root),
                PlanKind::Settings,
                plan,
                dir.path().into(),
                vec![],
                &tx,
                &egui::Context::default()
            ),
            Outcome::Applied { result: Err(_), .. }
        ));
        assert!(!root.exists());
        fs::write(&ini, "[DlssNr]\nStage=1\n").unwrap();
        record(&root, dir.path(), entry).unwrap();
        let inverse = undo_plan(&root, dir.path(), Lang::En).unwrap();
        assert!(matches!(&inverse.steps()[0], Step::UnsetIni {old: Some(v), ..} if v == "1"));
    }
}

#[cfg(test)]
mod exact_undo_tests {
    use super::*;

    fn apply_and_record(target: &Path, root: &Path, edits: &[(String, String, String)]) {
        let plan = retoque_core::plan_settings(target, edits).unwrap();
        let (tx, _) = std::sync::mpsc::channel();
        assert!(matches!(
            apply_recorded(
                Some(root),
                PlanKind::Settings,
                plan,
                target.into(),
                vec![],
                &tx,
                &egui::Context::default()
            ),
            Outcome::Applied { result: Ok(()), .. }
        ));
    }

    #[test]
    fn lsfg_x2_roundtrip_restores_original_bytes_including_absent_section() {
        for original in [
            b"\xef\xbb\xbf;preserve\r\n[OptiFG]\r\nFGInput = auto \r\n[LSFG]\r\n;DllPath=comment\r\n".as_slice(),
            b"[OptiFG]\nFGInput=auto\n;no LSFG section\n",
            b"[OptiFG]\nFGInput=auto\n[LSFG]",
        ] {
            let dir = tempfile::tempdir().unwrap();
            let target = dir.path().join("game");
            let root = dir.path().join("history");
            fs::create_dir(&target).unwrap();
            let ini = target.join("OptiScaler.ini");
            fs::write(&ini, original).unwrap();
            apply_and_record(
                &target,
                &root,
                &[
                    ("LSFG".into(), "DllPath".into(), "C:\\Games\\LS\\lsfg-vk.dll".into()),
                    ("LSFG".into(), "ShaderDir".into(), "C:\\Shaders".into()),
                    ("OptiFG".into(), "FGInput".into(), "upscaler".into()),
                ],
            );
            let after = fs::read(&ini).unwrap();
            for lang in [Lang::En, Lang::Pt] {
                let undo = undo_plan(&root, &target, lang).unwrap();
                assert_eq!(fs::read(&ini).unwrap(), after, "planning cannot write");
                assert_eq!(
                    undo.steps()
                        .iter()
                        .filter(|s| matches!(s, Step::UnsetIni { .. }))
                        .count(),
                    2
                );
                assert_eq!(crate::i18n::diagnostic(lang, &undo.notes()[0]), t(lang, "undo.exact"));
                for step in undo.steps().iter().filter(|s| matches!(s, Step::UnsetIni { .. })) {
                    let (label, text, _) = crate::format::step_text(lang, step);
                    assert_eq!(label, t(lang, "step.remove"));
                    assert!(text.contains("[LSFG]"));
                }
                retoque_core::apply(&undo.into_verified().unwrap(), &mut |_| {}).unwrap();
                assert_eq!(fs::read(&ini).unwrap(), original);
                assert!(!target.join(".retoque").exists());
                fs::write(&ini, &after).unwrap();
            }
        }
    }

    #[test]
    fn exact_history_remembers_header_termination_and_duplicate_formatting() {
        for original in [
            b"[A]".as_slice(),
            b"[A]\n",
            b"",
            b"\n[A]\n",
            b"[A]\nk= one \r\n[A]\nk=two\n",
        ] {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().join("history");
            let ini = dir.path().join("OptiScaler.ini");
            fs::write(&ini, original).unwrap();
            apply_and_record(dir.path(), &root, &[("A".into(), "k".into(), "v".into())]);
            let undo = undo_plan(&root, dir.path(), Lang::En).unwrap();
            retoque_core::apply(&undo.into_verified().unwrap(), &mut |_| {}).unwrap();
            assert_eq!(fs::read(&ini).unwrap(), original);
        }
    }

    #[test]
    fn changed_file_and_legacy_entries_use_conservative_removal_and_localized_notice() {
        for legacy in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().join("history");
            let ini = dir.path().join("OptiScaler.ini");
            fs::write(&ini, b"[A]").unwrap();
            apply_and_record(dir.path(), &root, &[("A".into(), "k".into(), "v".into())]);
            if legacy {
                // Literal old schema: string `after`, no optional exact metadata.
                fs::write(
                    file(&root, dir.path()).unwrap(),
                    br#"{"entries":[{"time_unix":0,"changes":[{"section":"A","key":"k","before":null,"after":"v"}]}]}"#,
                )
                .unwrap();
            } else {
                fs::write(&ini, b"[A]\nk=v\n; external comment\r\n").unwrap();
            }
            for lang in [Lang::En, Lang::Pt] {
                let undo = undo_plan(&root, dir.path(), lang).unwrap();
                assert_eq!(
                    crate::i18n::diagnostic(lang, &undo.notes()[0]),
                    t(lang, "undo.conservative")
                );
                assert!(matches!(undo.steps()[0], Step::UnsetIni { .. }));
            }
            let undo = undo_plan(&root, dir.path(), Lang::En).unwrap();
            retoque_core::apply(&undo.into_verified().unwrap(), &mut |_| {}).unwrap();
            assert_eq!(
                fs::read(&ini).unwrap(),
                if legacy {
                    b"[A]\n".as_slice()
                } else {
                    b"[A]\n; external comment\r\n"
                }
            );
        }
    }

    #[test]
    fn repeated_key_edits_reverse_in_order_and_recorded_unset_can_be_undone() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("history");
        let ini = dir.path().join("OptiScaler.ini");
        fs::write(&ini, b"[A]").unwrap();
        apply_and_record(
            dir.path(),
            &root,
            &[
                ("A".into(), "k".into(), "one".into()),
                ("A".into(), "k".into(), "two".into()),
            ],
        );
        let undo = undo_plan(&root, dir.path(), Lang::En).unwrap();
        let entry = capture(&undo);
        retoque_core::apply(&undo.into_verified().unwrap(), &mut |_| {}).unwrap();
        assert_eq!(fs::read(&ini).unwrap(), b"[A]");
        record(&root, dir.path(), entry).unwrap();
        let redo = undo_plan(&root, dir.path(), Lang::En).unwrap();
        retoque_core::apply(&redo.into_verified().unwrap(), &mut |_| {}).unwrap();
        assert_eq!(
            retoque_core::ini::Ini::parse(&fs::read(&ini).unwrap()).get("A", "k"),
            Some("two")
        );
    }

    #[test]
    fn insertion_history_does_not_copy_unrelated_ini_contents() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("history");
        let ini = dir.path().join("OptiScaler.ini");
        let original = format!(";{}\n[A]", "x".repeat(100_000));
        fs::write(&ini, original.as_bytes()).unwrap();
        apply_and_record(dir.path(), &root, &[("A".into(), "k".into(), "v".into())]);
        assert!(fs::metadata(file(&root, dir.path()).unwrap()).unwrap().len() < 2048);
        let undo = undo_plan(&root, dir.path(), Lang::En).unwrap();
        retoque_core::apply(&undo.into_verified().unwrap(), &mut |_| {}).unwrap();
        assert_eq!(fs::read(&ini).unwrap(), original.as_bytes());
    }

    #[test]
    fn no_exact_snapshot_if_changed_after_apply_before_history_write() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("history");
        let ini = dir.path().join("OptiScaler.ini");
        fs::write(&ini, b"[A]").unwrap();
        let plan = retoque_core::plan_settings(dir.path(), &[("A".into(), "k".into(), "v".into())]).unwrap();
        let entry = capture(&plan);
        retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
        fs::write(&ini, b"[A]\nk=v\n; later\n").unwrap();
        record(&root, dir.path(), entry).unwrap();
        assert!(
            load(&file(&root, dir.path()).unwrap()).unwrap().entries[0]
                .exact
                .is_empty()
        );
    }
}
