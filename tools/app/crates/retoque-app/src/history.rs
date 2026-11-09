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
    pub after: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Applied {
    pub time_unix: u64,
    pub changes: Vec<Change>,
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
                } if old.as_ref() != Some(value) => Some(Change {
                    section: section.clone(),
                    key: key.clone(),
                    before: old.clone(),
                    after: value.clone(),
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
/// Missing original keys restore `auto`; removal is not an operation supported by the INI engine.
pub(crate) fn undo_plan(root: &Path, target: &Path, lang: Lang) -> Result<Plan, AppError> {
    let path = file(root, target).map_err(AppError::from)?;
    let history = load(&path).map_err(|e| AppError::io(path, e))?;
    let entry = history
        .entries
        .last()
        .ok_or_else(|| AppError::from(t(lang, "undo.empty")))?;
    let edits: Vec<Edit> = entry
        .changes
        .iter()
        .map(|c| {
            (
                c.section.clone(),
                c.key.clone(),
                c.before.clone().unwrap_or_else(|| "auto".into()),
            )
        })
        .collect();
    let plan = retoque_core::plan_settings(target, &edits).map_err(AppError::from)?;
    if !plan.refusals().is_empty() {
        return Ok(plan);
    }
    // Inspect the old values *inside* the sealed plan, never an earlier UI snapshot/read.
    // Missing steps (already reverted or externally changed to the inverse) also refuse undo.
    for c in &entry.changes {
        let matches = plan.steps().iter().any(|s| {
            matches!(s, Step::SetIni { section, key, old, .. }
            if section == &c.section && key == &c.key && old.as_ref() == Some(&c.after))
        });
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
    fn failed_apply_never_records_and_missing_before_restores_auto() {
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
        assert!(matches!(&inverse.steps()[0], Step::SetIni {value, ..} if value == "auto"));
    }
}
