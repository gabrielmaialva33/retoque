//! ReShade/add-on lifecycle on the ordinary sealed plan and v1 journal engine.
//! Configuration semantics: crosire/reshade v6.8.0 source/{addon_manager,ini_file}.cpp.
use super::*;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

pub const DLL: &str = "ReShade64.dll";
pub const CONFIG: &str = "ReShade.ini";
const STATE: &str = ".retoque/reshade.ini";
// Undo of the one ReShade.ini setting the install makes, [INPUT] InputProcessing (see menu_input_blocked).
const INPUT_STATE: &str = ".retoque/reshade-input.ini";
const MAX_DLL: u64 = 128 * 1024 * 1024;
const MAX_INI: u64 = 2 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum Build {
    Full,
    Limited,
    #[default]
    Unknown,
}
/// Minimum static architecture check; it never certifies that the DLL will load successfully.
pub fn is_x64_dll(bytes: &[u8]) -> bool {
    let Some(offset) = bytes
        .get(60..64)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()) as usize)
    else {
        return false;
    };
    let Some(header) = offset.checked_add(26).and_then(|end| bytes.get(offset..end)) else {
        return false;
    };
    bytes.starts_with(b"MZ")
        && header.starts_with(b"PE\0\0")
        && header[4..6] == [0x64, 0x86]
        && u16::from_le_bytes([header[20], header[21]]) >= 2
        && u16::from_le_bytes([header[22], header[23]]) & 0x2000 != 0
        && header[24..26] == [0x0b, 0x02]
}
pub fn build(bytes: &[u8]) -> Build {
    let has = |s: &[u8]| bytes.windows(s.len()).any(|w| w == s);
    match (has(b"LoadFromDllMain"), has(b"only limited add-on functionality")) {
        (true, false) => Build::Full,
        (false, true) => Build::Limited,
        _ => Build::Unknown,
    }
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Addon {
    pub file: String,
    pub path: PathBuf,
    pub size: u64,
    pub version: Option<String>,
    pub product: Option<String>,
    pub name: String,
    pub enabled: bool,
    pub imported: bool,
    pub tracked: bool,
    pub nr_candidate: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NrOwner {
    Off,
    Retoque,
    Addon,
}
/// `[DlssNr] YieldToAddons` keeps Retoque's NR beside a neural add-on only when it is false (or 0); absent or
/// `auto` means the runtime hands NR to the add-on for the session (retoque/dlssnr/DlssNr_Consumers.h).
pub fn yields_to_addons(value: Option<&str>) -> bool {
    !value.is_some_and(|v| v.trim().eq_ignore_ascii_case("false") || v.trim() == "0")
}
/// The add-ons Retoque's NR hands over to, as file-name markers: the runtime's list in
/// retoque/dlssnr/DlssNr_ConsumerMarkers.h (`kCompetitors`), matched the same way (ASCII case-insensitive, anywhere in
/// the file name). A test keeps the two lists equal.
pub const NR_ADDON_MARKERS: [&str; 7] = [
    "deep-fried-chicken",
    "renodx-dlss",
    "renodx_dlss",
    "dlss5-feed",
    "dlss5_feed",
    "amdlss5",
    "nr_native_adapter",
];
/// Whether the runtime would hand NR to an add-on with this file name.
pub fn is_nr_addon(file: &str) -> bool {
    let lower = file.to_ascii_lowercase();
    NR_ADDON_MARKERS.iter().any(|marker| lower.contains(marker))
}
pub fn nr_owner(nr_on: bool, yield_to_addons: Option<&str>, addons: &[Addon]) -> NrOwner {
    if !nr_on {
        NrOwner::Off
    } else if yields_to_addons(yield_to_addons) && addons.iter().any(|a| a.enabled && a.nr_candidate) {
        NrOwner::Addon
    } else {
        NrOwner::Retoque
    }
}
/// Whether ReShade's overlay takes the mouse and keyboard from everything else in the game while it is open.
/// ReShade 6.8 defaults `[INPUT] InputProcessing` to 2 (block all input while the overlay is visible); its message
/// hooks sit below OptiScaler's (ReShade is loaded first), so in that mode the Retoque menu never sees the mouse while
/// both are open. 1 blocks only while the cursor is over ReShade's own windows; 0 never blocks.
pub fn menu_input_blocked(config: &[u8]) -> bool {
    !matches!(
        Ini::parse_sensitive(config)
            .get("INPUT", "InputProcessing")
            .map(str::trim),
        Some("0" | "1")
    )
}
/// menu_input_blocked for the game's own ReShade.ini (absent reads as ReShade's default, which blocks).
pub fn menu_input_blocked_in(game: &Path) -> Result<bool, Error> {
    Ok(menu_input_blocked(&read_config(&canonical(game)?, CONFIG)?.1))
}
pub fn addon_filename(name: &str) -> bool {
    safe_rel(name)
        && !name.contains(['/', '\\', '\r', '\n'])
        && (name.ends_with(".addon64") || name.ends_with(".addon"))
}
fn bounded(path: &Path, limit: u64) -> Result<Vec<u8>, Error> {
    if !regular(path) {
        return Err(usage_error("ReShade input is a link or not a regular file"));
    }
    let mut bytes = Vec::new();
    fs::File::open(path)
        .map_err(|e| io(path, e))?
        .take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| io(path, e))?;
    if bytes.len() as u64 > limit {
        return Err(usage_error("ReShade input exceeds size limit"));
    }
    Ok(bytes)
}
fn unique(game: &Path, name: &str) -> Result<String, Error> {
    check_parents(game, name)?;
    let names = variants(game, name);
    if names.len() > 1 {
        return Err(usage_error("ambiguous ReShade filename casing"));
    }
    Ok(names.into_iter().next().unwrap_or_else(|| name.into()))
}
fn read_config(game: &Path, name: &str) -> Result<(String, Vec<u8>), Error> {
    let file = unique(game, name)?;
    let path = game.join(&file);
    Ok((
        file,
        if exists(&path) {
            bounded(&path, MAX_INI)?
        } else {
            Vec::new()
        },
    ))
}
pub fn decode_list(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut item = String::new();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if c == ',' && chars.peek() == Some(&',') {
            chars.next();
            item.push(',');
        } else if c == ',' {
            out.push(std::mem::take(&mut item));
        } else {
            item.push(c);
        }
    }
    if !text.is_empty() {
        out.push(item);
    }
    out
}
fn encode_list(items: &[String]) -> String {
    items
        .iter()
        .filter(|s| !s.is_empty())
        .map(|s| s.replace(',', ",,"))
        .collect::<Vec<_>>()
        .join(",")
}
fn config_values(bytes: &[u8], key: &str) -> Vec<String> {
    // Unlike OptiScaler, ReShade appends duplicate values and compares case-sensitively.
    let ini = Ini::parse_sensitive(bytes);
    ini.values("ADDON", key)
        .iter()
        .filter_map(|b| std::str::from_utf8(b).ok())
        .flat_map(decode_list)
        .collect()
}
fn addon_dir(game: &Path, bytes: &[u8]) -> Result<PathBuf, Error> {
    let Some(value) = config_values(bytes, "AddonPath")
        .into_iter()
        .next()
        .filter(|s| !s.is_empty())
    else {
        return Ok(game.into());
    };
    let value = value.replace('\\', "/");
    #[cfg(not(windows))]
    let value = if value.starts_with("Z:/") || value.starts_with("z:/") {
        value[2..].to_owned()
    } else if value.contains(':') || value.starts_with("//") {
        return Err(usage_error("AddonPath uses an unresolved Wine drive or network path"));
    } else {
        value
    };
    Ok(game.join(value))
}
fn disabled(token: &str, file: &str, name: &str) -> bool {
    // Filename match prevents loading before registration, independent of the displayed name.
    token.split_once('@').map_or(token == name, |(_, f)| f == file)
}
fn addon_state(name: &str) -> String {
    format!(".retoque/addon-{:x}.ini", Sha256::digest(name.as_bytes()))
}

pub fn addons(game: &Path) -> Result<Vec<Addon>, Error> {
    let game = canonical(game)?;
    let (_, bytes) = read_config(&game, CONFIG)?;
    let directory = addon_dir(&game, &bytes)?;
    if linked(&directory) {
        return Err(usage_error("AddonPath is a link"));
    }
    let entries = match fs::read_dir(&directory) {
        Ok(v) => v,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(io(&directory, e)),
    };
    let disabled_list = config_values(&bytes, "DisabledAddons");
    let receipt = load_receipt(&game)?.unwrap_or_else(empty_receipt);
    let mut result = Vec::new();
    let mut names = BTreeSet::new();
    let mut total = 0u64;
    for (n, entry) in entries.enumerate() {
        if n >= 8192 {
            return Err(usage_error("add-on scan limit exceeded"));
        }
        let entry = entry.map_err(|e| io(&directory, e))?;
        let file = entry
            .file_name()
            .into_string()
            .map_err(|_| usage_error("non UTF-8 add-on filename"))?;
        if !file.ends_with(".addon") && !file.ends_with(".addon64") {
            continue;
        }
        if !names.insert(file.to_ascii_lowercase()) {
            return Err(usage_error("ambiguous add-on filename casing"));
        }
        if !regular(&entry.path()) {
            return Err(usage_error("add-on is a link or not a regular file"));
        }
        let metadata = fs::metadata(entry.path()).map_err(|e| io(&entry.path(), e))?;
        total = total.saturating_add(metadata.len());
        if total > 512 * 1024 * 1024 {
            return Err(usage_error("add-on inspection exceeds size budget"));
        }
        let data = bounded(&entry.path(), MAX_DLL)?;
        let details = pe_version::read_details(&mut std::io::Cursor::new(&data));
        let product = details.as_ref().and_then(|d| d.product.clone());
        let name = crate::addon_pe::name(&data)
            .or_else(|| product.clone())
            .unwrap_or_else(|| Path::new(&file).file_stem().unwrap().to_string_lossy().into_owned());
        result.push(Addon {
            enabled: !disabled_list.iter().any(|t| disabled(t, &file, &name)),
            nr_candidate: is_nr_addon(&file),
            tracked: exists(&game.join(addon_state(&file))),
            imported: exists(&game.join(addon_state(&file)))
                && receipt
                    .records
                    .iter()
                    .any(|r| matches!(r, Record::File {path,..} if path == &file)),
            version: details.map(|d| d.version),
            product,
            name,
            file,
            path: entry.path(),
            size: metadata.len(),
        });
    }
    result.sort_by(|a, b| a.file.cmp(&b.file));
    Ok(result)
}
pub fn installed_build(game: &Path) -> Result<Option<Build>, Error> {
    let game = canonical(game)?;
    for name in [DLL, "d3d12-ReShade64.dll"] {
        let file = unique(&game, name)?;
        if exists(&game.join(&file)) {
            return Ok(Some(build(&bounded(&game.join(file), MAX_DLL)?)));
        }
    }
    Ok(None)
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct EditRecord {
    file: String,
    section: String,
    key: String,
    old: Option<String>,
    new: Option<String>,
    undo: ini::IniUndo,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct State {
    schema: u32,
    file: String,
    // An identical preexisting file is never claimed or removed.
    created: bool,
    #[serde(default)]
    before_sha: Option<String>,
    #[serde(default)]
    addon_name: Option<String>,
    ini_existed: bool,
    original_disabled: Vec<String>,
    edits: Vec<EditRecord>,
}
fn load_state(game: &Path, path: &str, receipt: &Receipt) -> Result<Option<State>, Error> {
    check_parents(game, path)?;
    if !exists(&game.join(path)) {
        return Ok(None);
    }
    let bytes = bounded(&game.join(path), MAX_INI)?;
    if !owned(receipt, path, &format!("{:x}", Sha256::digest(&bytes))) {
        return Err(usage_error("ReShade state is not unchanged receipt-owned data"));
    }
    let ini = Ini::parse(&bytes);
    let state: State = serde_json::from_str(
        ini.get("State", "Data")
            .ok_or_else(|| usage_error("invalid ReShade state"))?,
    )
    .map_err(|_| usage_error("invalid ReShade state"))?;
    if state.schema != 1
        || (state.file != DLL && state.file != CONFIG && !addon_filename(&state.file))
        || state.edits.len() > 1024
    {
        return Err(usage_error("invalid ReShade state"));
    }
    Ok(Some(state))
}
fn save_state(plan: &mut Plan, path: &str, state: &State) -> Result<(), Error> {
    let value = serde_json::to_string(state).map_err(|e| Error::Other(e.to_string()))?;
    if value.len() > MAX_INI as usize / 2 {
        return Err(usage_error("ReShade undo history exceeds limit"));
    }
    let old = if exists(&plan.game_dir.join(path)) {
        Ini::parse(&bounded(&plan.game_dir.join(path), MAX_INI)?)
            .get("State", "Data")
            .map(str::to_owned)
    } else {
        None
    };
    if old.as_deref() != Some(&value) {
        plan.steps.push(Step::SetIni {
            file: path.into(),
            section: "State".into(),
            key: "Data".into(),
            old,
            value,
        });
    }
    Ok(())
}
fn save_state_first(plan: &mut Plan, path: &str, state: &State) -> Result<(), Error> {
    let count = plan.steps.len();
    save_state(plan, path, state)?;
    if plan.steps.len() > count {
        let step = plan.steps.pop().unwrap();
        plan.steps.insert(0, step);
        plan.execution.forget = std::mem::take(&mut plan.execution.forget)
            .into_iter()
            .map(|(i, r)| (i + 1, r))
            .collect();
    }
    Ok(())
}
fn note_edit(plan: &mut Plan, record: &EditRecord, inverse: bool, exact: bool) {
    let index = plan.steps.len();
    plan.execution
        .user_ini
        .insert(index, if exact { Some(record.undo.clone()) } else { None });
    let (old, value) = if inverse {
        (&record.new, &record.old)
    } else {
        (&record.old, &record.new)
    };
    plan.steps.push(match value {
        Some(value) => Step::SetIni {
            file: record.file.clone(),
            section: record.section.clone(),
            key: record.key.clone(),
            old: old.clone(),
            value: value.clone(),
        },
        None => Step::UnsetIni {
            file: record.file.clone(),
            section: record.section.clone(),
            key: record.key.clone(),
            old: old.clone(),
        },
    });
}
fn record_edit(bytes: &[u8], file: &str, section: &str, key: &str, value: Option<String>) -> Option<EditRecord> {
    let mut ini = if file.eq_ignore_ascii_case(CONFIG) {
        Ini::parse_sensitive(bytes)
    } else {
        Ini::parse(bytes)
    };
    let old = ini.get(section, key).map(str::to_owned);
    let undo = ini.edit_with_undo(section, key, value.as_deref())?;
    Some(EditRecord {
        file: file.into(),
        section: section.into(),
        key: key.into(),
        old,
        new: value,
        undo,
    })
}
fn observe(plan: &mut Plan, path: &Path) -> Result<(), Error> {
    plan.execution.snapshots.insert(path.into(), fingerprint(path)?);
    plan.execution.observed_files.insert(path.into(), observed_file(path)?);
    Ok(())
}
fn begin(game: &Path, remove: bool) -> Result<(Plan, Receipt), Error> {
    let mut plan = new_plan(
        game,
        if remove {
            Operation::Uninstall
        } else {
            Operation::Install
        },
    )?;
    let receipt = load_receipt(&plan.game_dir)?.unwrap_or_else(empty_receipt);
    plan.execution.component_completion = Some(receipt.status);
    plan.execution.component_only = remove;
    plan.execution.lifecycle_removal = remove;
    Ok((plan, receipt))
}
fn copy(plan: &mut Plan, receipt: &Receipt, from: &Path, name: &str, replace_owned: bool) -> Result<bool, Error> {
    let file = unique(&plan.game_dir, name)?;
    if file != name {
        return Err(usage_error("conflicting ReShade/add-on destination casing"));
    }
    let src = bounded(from, MAX_DLL)?;
    if !is_x64_dll(&src) {
        return Err(usage_error("ReShade/add-on input is not an x64 PE DLL"));
    }
    let to = plan.game_dir.join(name);
    if exists(&to) {
        let dst = bounded(&to, MAX_DLL)?;
        if src == dst {
            let index = plan.steps.len();
            if !owned(receipt, name, &hash(&to)?) {
                plan.execution.forget.insert(
                    index,
                    Record::File {
                        path: name.into(),
                        sha256: String::new(),
                    },
                );
            }
            plan.steps.push(Step::Keep {
                path: name.into(),
                why: "identical existing file; preserve its original ownership".into(),
            });
            return Ok(false);
        }
        if !replace_owned {
            plan.refusals.push(foreign(name));
            return Ok(false);
        }
        if !owned(receipt, name, &hash(&to)?) {
            if receipt
                .records
                .iter()
                .any(|r| matches!(r,Record::File{path,..} if path==name))
                || build(&dst) == Build::Unknown
            {
                plan.refusals.push(foreign(name));
                return Ok(false);
            }
            plan.steps.push(Step::Backup {
                path: name.into(),
                copy: format!("{}/{name}", backup_dir(&plan.game_dir)),
            });
        }
    }
    plan.steps.push(Step::Copy {
        from: from.into(),
        to: name.into(),
    });
    Ok(true)
}
fn loader_guards(plan: &mut Plan) -> Result<(), Error> {
    let game = plan.game_dir.clone();
    for name in PROXY_NAMES.into_iter().chain([
        "dinput8.dll",
        "opengl32.dll",
        "d3d9.dll",
        "SpecialK64.dll",
        "d3d12-ReShade64.dll",
    ]) {
        observe(plan, &game.join(name))?;
        for file in variants(&game, name) {
            let path = game.join(&file);
            observe(plan, &path)?;
            if marker(&path, MARK_OPTI) {
                continue;
            }
            let special = file.eq_ignore_ascii_case("SpecialK64.dll")
                || marker(&path, b"Special K")
                || marker(&path, b"SpecialK");
            if special {
                plan.refusals
                    .push(Refusal::Usage("Special K is present; it owns ReShade loading".into()));
            } else if marker(&path, b"ReShade") || file.eq_ignore_ascii_case("d3d12-ReShade64.dll") {
                plan.refusals.push(Refusal::Usage(
                    "another ReShade loader is present; use the existing keep_reshade migration first".into(),
                ));
            }
        }
    }
    Ok(())
}
// The [INPUT] InputProcessing=1 edit and its undo state, or None when ReShade already lets the menu have the mouse.
fn menu_input_edit(game: &Path, receipt: &Receipt) -> Result<Option<(State, EditRecord)>, Error> {
    let (file, bytes) = read_config(game, CONFIG)?;
    if !menu_input_blocked(&bytes) {
        return Ok(None);
    }
    let Some(edit) = record_edit(&bytes, &file, "INPUT", "InputProcessing", Some("1".into())) else {
        return Ok(None);
    };
    let mut state = load_state(game, INPUT_STATE, receipt)?.unwrap_or(State {
        schema: 1,
        file: CONFIG.into(),
        created: false,
        before_sha: None,
        addon_name: None,
        ini_existed: exists(&game.join(&file)),
        original_disabled: Vec::new(),
        edits: Vec::new(),
    });
    state.edits.push(edit.clone());
    Ok(Some((state, edit)))
}
/// Lets the Retoque menu use the mouse while ReShade's overlay is open: `[INPUT] InputProcessing=1` in ReShade.ini,
/// reviewed and undone with the ReShade component (or alone, when nothing else changes).
pub fn plan_menu_input(game: &Path) -> Result<Plan, Error> {
    let (mut plan, receipt) = begin(game, false)?;
    let game = plan.game_dir.clone();
    if let Some((state, edit)) = menu_input_edit(&game, &receipt)? {
        save_state(&mut plan, INPUT_STATE, &state)?;
        note_edit(&mut plan, &edit, false, false);
    }
    seal(plan, &receipt)
}
pub fn plan_install(game: &Path, exe_dir: &Path, source: &Path) -> Result<Plan, Error> {
    let (mut plan, receipt) = begin(game, false)?;
    if canonical(exe_dir)? != plan.game_dir {
        plan.refusals.push(Refusal::Usage(
            "ReShade requires the proxy and executable in the same directory".into(),
        ));
    }
    loader_guards(&mut plan)?;
    let game = plan.game_dir.clone();
    if status(&game)?.optiscaler_proxies.is_empty() {
        plan.refusals
            .push(Refusal::Usage("install Retoque before its ReShade component".into()));
    }
    let (file, bytes) = read_ini_at(&game)?.ok_or_else(|| usage_error("OptiScaler.ini is missing"))?;
    let file = file.file_name().unwrap().to_string_lossy().into_owned();
    let mut state = load_state(&game, STATE, &receipt)?.unwrap_or(State {
        schema: 1,
        file: DLL.into(),
        created: false,
        before_sha: if exists(&game.join(DLL)) {
            Some(hash(&game.join(DLL))?)
        } else {
            None
        },
        addon_name: None,
        ini_existed: true,
        original_disabled: Vec::new(),
        edits: Vec::new(),
    });
    let created = copy(&mut plan, &receipt, source, DLL, true)?;
    state.created |= created;
    let edit = record_edit(&bytes, &file, "Plugins", "LoadReshade", Some("true".into()));
    if let Some(edit) = &edit {
        state.edits.push(edit.clone());
    }
    let input = menu_input_edit(&game, &receipt)?;
    // Write undo evidence before changing the user's INIs, including interrupted installs. Each insert goes first, so
    // the component's own state (which uninstall looks for) is written before the input state.
    if let Some((input_state, _)) = &input {
        save_state_first(&mut plan, INPUT_STATE, input_state)?;
    }
    save_state_first(&mut plan, STATE, &state)?;
    if let Some(edit) = edit {
        note_edit(&mut plan, &edit, false, false);
    }
    if let Some((_, edit)) = input {
        note_edit(&mut plan, &edit, false, false);
    }
    if build(&bounded(source, MAX_DLL)?) != Build::Full {
        plan.notes
            .push("ReShade build is limited or unknown; full add-on support is not established".into());
    }
    seal(plan, &receipt)
}
fn restore_edits(plan: &mut Plan, state: &State, virtual_inis: &mut BTreeMap<String, Vec<u8>>) -> Result<(), Error> {
    if state.edits.is_empty() {
        return Ok(());
    }
    let file = &state.edits[0].file;
    if !file.eq_ignore_ascii_case(CONFIG) && !file.eq_ignore_ascii_case(INI_NAME) {
        return Err(usage_error("invalid lifecycle INI path"));
    }
    check_parents(&plan.game_dir, file)?;
    let bytes = if let Some(bytes) = virtual_inis.get(file) {
        bytes.clone()
    } else if exists(&plan.game_dir.join(file)) {
        bounded(&plan.game_dir.join(file), MAX_INI)?
    } else {
        Vec::new()
    };
    let mut ini = if file.eq_ignore_ascii_case(CONFIG) {
        Ini::parse_sensitive(&bytes)
    } else {
        Ini::parse(&bytes)
    };
    for record in state.edits.iter().rev() {
        if record.file != *file
            || !matches!(
                (record.section.as_str(), record.key.as_str()),
                ("Plugins", "LoadReshade") | ("ADDON", "DisabledAddons") | ("INPUT", "InputProcessing")
            )
        {
            return Err(usage_error("invalid lifecycle edit identity"));
        }
        if record.undo.matches_before(&ini.to_bytes()) {
            continue;
        }
        if ini.undo(&record.undo) {
            note_edit(plan, record, true, true);
        } else if file.eq_ignore_ascii_case(CONFIG) && record.key == "DisabledAddons" {
            // Another add-on or the user changed the list: restore only this file's original entries.
            let mut values = config_values(&ini.to_bytes(), "DisabledAddons");
            let fallback = Path::new(&state.file).file_stem().unwrap().to_string_lossy();
            let name = state.addon_name.as_deref().unwrap_or(&fallback);
            values.retain(|t| !disabled(t, &state.file, name));
            values.extend(state.original_disabled.clone());
            if let Some(edit) = record_edit(
                &ini.to_bytes(),
                file,
                "ADDON",
                "DisabledAddons",
                Some(encode_list(&values)),
            ) {
                note_edit(plan, &edit, false, false);
                ini.set("ADDON", "DisabledAddons", edit.new.as_deref().unwrap());
            }
            plan.notes
                .push("Conservative INI undo: other edits are preserved; restoration is not byte-for-byte".into());
            break;
        } else {
            let current = ini.get(&record.section, &record.key);
            if current == record.old.as_deref() {
                continue;
            } // mutation did not run before interruption
            if current != record.new.as_deref() {
                if record.key == "InputProcessing" {
                    // The user chose another input mode in ReShade's own settings since: theirs stands.
                    plan.notes.push(
                        "ReShade's input processing was changed since install; the current choice is kept".into(),
                    );
                    continue;
                }
                return Err(usage_error(
                    "LoadReshade changed since the reviewed install; preserve the undo state",
                ));
            }
            note_edit(plan, record, true, false);
            if let Some(v) = &record.old {
                ini.set(&record.section, &record.key, v);
            } else {
                ini.unset(&record.section, &record.key);
            }
            plan.notes
                .push("Conservative INI undo: other edits are preserved; restoration is not byte-for-byte".into());
        }
    }
    if !state.ini_existed && ini.to_bytes().is_empty() && exists(&plan.game_dir.join(file)) {
        // Only an INI we created and exactly undid may disappear; never remove a user preset/config.
        plan.steps.push(Step::Remove { path: file.clone() });
    }
    virtual_inis.insert(file.clone(), ini.to_bytes());
    Ok(())
}
fn remove(game: &Path, state_path: &str, expected: &str) -> Result<Plan, Error> {
    let (mut plan, receipt) = begin(game, true)?;
    let state = load_state(&plan.game_dir, state_path, &receipt)?
        .ok_or_else(|| usage_error("component was not imported by Retoque"))?;
    if state.file != expected {
        return Err(usage_error("component state identity mismatch"));
    }
    if state.created {
        let path = plan.game_dir.join(&state.file);
        if exists(&path) {
            if !regular(&path) {
                return Err(usage_error("imported file changed; no files were removed"));
            }
            let current = hash(&path)?;
            if owned(&receipt, &state.file, &current) {
                plan.steps.push(Step::Remove {
                    path: state.file.clone(),
                });
            } else if state.before_sha.as_ref() != Some(&current) {
                return Err(usage_error("imported file changed; no files were removed"));
            }
        } else if let Some(record) = receipt
            .records
            .iter()
            .find(|r| matches!(r,Record::File {path,..} if path == &state.file))
        {
            plan.execution.forget.insert(plan.steps.len(), record.clone());
            plan.steps.push(Step::Keep {
                path: state.file.clone(),
                why: "already gone".into(),
            });
        }
    }
    for record in receipt.records.iter().rev() {
        if let Record::Backup { path, copy, sha256 } = record
            && path == &state.file
        {
            let source = plan.game_dir.join(copy);
            if !regular(&source) || hash(&source)? != *sha256 {
                return Err(usage_error("ReShade backup changed; preserve the receipt"));
            }
            if exists(&plan.game_dir.join(path))
                && !plan.steps.iter().any(|s| matches!(s,Step::Remove {path:p} if p==path))
            {
                if hash(&plan.game_dir.join(path))? != *sha256 {
                    return Err(usage_error("ReShade backup changed; preserve the receipt"));
                }
                plan.steps.push(Step::Remove { path: copy.clone() });
                plan.execution.forget.insert(plan.steps.len(), record.clone());
                plan.steps.push(Step::Keep {
                    path: copy.clone(),
                    why: "original already restored".into(),
                });
            } else {
                plan.steps.push(Step::Restore {
                    copy: copy.clone(),
                    to: path.clone(),
                });
            }
        }
    }
    restore_edits(&mut plan, &state, &mut BTreeMap::new())?;
    plan.steps.push(Step::Remove {
        path: state_path.into(),
    });
    if state_path == STATE
        && let Some(input) = load_state(&plan.game_dir, INPUT_STATE, &receipt)?
    {
        restore_edits(&mut plan, &input, &mut BTreeMap::new())?;
        plan.steps.push(Step::Remove {
            path: INPUT_STATE.into(),
        });
    }
    seal(plan, &receipt)
}
pub fn plan_uninstall(game: &Path) -> Result<Plan, Error> {
    remove(game, STATE, DLL)
}
fn source_addon_name(source: &Path) -> Result<String, Error> {
    let bytes = bounded(source, MAX_DLL)?;
    Ok(crate::addon_pe::name(&bytes)
        .or_else(|| pe_version::read_details(&mut std::io::Cursor::new(&bytes)).and_then(|v| v.product))
        .unwrap_or_else(|| source.file_stem().unwrap_or_default().to_string_lossy().into_owned()))
}
pub fn plan_add(game: &Path, source: &Path) -> Result<Plan, Error> {
    let name = source
        .file_name()
        .and_then(|s| s.to_str())
        .filter(|s| addon_filename(s))
        .ok_or_else(|| usage_error("select an .addon64 or .addon file with a safe filename"))?;
    let (mut plan, receipt) = begin(game, false)?;
    let game = plan.game_dir.clone();
    let (ini_name, bytes) = read_config(&game, CONFIG)?;
    if canonical(&addon_dir(&game, &bytes)?)? != game {
        return Err(usage_error(
            "AddonPath differs from the game folder; importing here would not load the add-on",
        ));
    }
    observe(&mut plan, &game.join(&ini_name))?;
    let state_path = addon_state(name);
    let previous = load_state(&game, &state_path, &receipt)?;
    let created = copy(&mut plan, &receipt, source, name, false)?;
    let display_name = source_addon_name(source)?;
    let mut state = previous.unwrap_or(State {
        schema: 1,
        file: name.into(),
        created: false,
        before_sha: None,
        addon_name: Some(display_name.clone()),
        ini_existed: exists(&game.join(&ini_name)),
        original_disabled: config_values(&bytes, "DisabledAddons")
            .into_iter()
            .filter(|t| disabled(t, name, &display_name))
            .collect(),
        edits: Vec::new(),
    });
    state.created |= created;
    save_state_first(&mut plan, &state_path, &state)?;
    seal(plan, &receipt)
}
pub fn plan_remove(game: &Path, name: &str) -> Result<Plan, Error> {
    if !addon_filename(name) {
        return Err(usage_error("invalid add-on filename"));
    }
    remove(game, &addon_state(name), name)
}
pub fn plan_enabled(game: &Path, name: &str, enabled: bool) -> Result<Plan, Error> {
    if !addon_filename(name) {
        return Err(usage_error("invalid add-on filename"));
    }
    let (mut plan, receipt) = begin(game, false)?;
    let game = plan.game_dir.clone();
    let item = addons(&game)?
        .into_iter()
        .find(|a| a.file == name)
        .ok_or_else(|| usage_error("add-on is not in ReShade's AddonPath"))?;
    observe(&mut plan, &item.path)?;
    let (file, bytes) = read_config(&game, CONFIG)?;
    let state_path = addon_state(name);
    let mut values = config_values(&bytes, "DisabledAddons");
    let mut state = load_state(&game, &state_path, &receipt)?.unwrap_or(State {
        schema: 1,
        file: name.into(),
        created: false,
        before_sha: None,
        addon_name: Some(item.name.clone()),
        ini_existed: exists(&game.join(&file)),
        original_disabled: values
            .iter()
            .filter(|t| disabled(t, name, &item.name))
            .cloned()
            .collect(),
        edits: Vec::new(),
    });
    // Import does not create ReShade.ini. A user may create it before the first toggle;
    // in that case even an empty file belongs to them and must survive undo.
    if state.edits.is_empty() {
        state.ini_existed |= exists(&game.join(&file));
    }
    values.retain(|t| !disabled(t, name, &item.name));
    if !enabled {
        values.push(format!("@{name}"));
    }
    let edit = record_edit(&bytes, &file, "ADDON", "DisabledAddons", Some(encode_list(&values)));
    if let Some(edit) = edit {
        state.edits.push(edit.clone());
        save_state(&mut plan, &state_path, &state)?;
        note_edit(&mut plan, &edit, false, false);
    }
    seal(plan, &receipt)
}

pub fn managed(game: &Path) -> Result<bool, Error> {
    let game = canonical(game)?;
    let receipt = load_receipt(&game)?.unwrap_or_else(empty_receipt);
    Ok(load_state(&game, STATE, &receipt)?.is_some())
}

// Full app/core uninstall restores the same INI changes before consuming ordinary v1 file records.
pub(super) fn before_uninstall(plan: &mut Plan, receipt: &Receipt) -> Result<(), Error> {
    let mut virtual_inis = BTreeMap::new();
    for record in receipt.records.iter().rev() {
        if let Record::File { path, .. } = record
            && (path == STATE || path == INPUT_STATE || (path.starts_with(".retoque/addon-") && path.ends_with(".ini")))
            && let Some(state) = load_state(&plan.game_dir, path, receipt)?
        {
            restore_edits(plan, &state, &mut virtual_inis)?;
        }
    }
    Ok(())
}
