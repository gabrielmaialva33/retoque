//! Generation-cached projections shared by sidebar, keyboard navigation and home cards.
use crate::app::{InstallState, Lang, install_state, runtime_short, store_name};
use retoque_core::Status;
use retoque_discovery::{Artwork, Game};
use std::{
    collections::{BTreeMap, HashMap},
    path::{Path, PathBuf},
    sync::RwLock,
};

/// What a scan saw in a candidate folder: Retoque's files (a receipt, OptiScaler under a proxy name or beside it),
/// and whether the receipt there is only the DLSS update's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Seen {
    retoque: bool,
    dlss_receipt: bool,
}

/// Telling takes reading the folder, and a proxy name another program holds (ReShade's dxgi.dll) is read whole.
/// The library is projected again after every background answer, on the UI thread, so the jobs that read a folder
/// anyway leave what they saw here: the scan for every folder, a status read for its own.
static SEEN: RwLock<BTreeMap<PathBuf, Seen>> = RwLock::new(BTreeMap::new());

fn look(target: &retoque_discovery::TargetDir) -> Seen {
    Seen {
        dlss_receipt: retoque_discovery::is_standalone_dlss_receipt(&target.path),
        retoque: retoque_discovery::target_has_retoque(target),
    }
}

/// Runs in the scan's job: what every candidate folder of these games holds now. A folder that is no longer listed
/// keeps its old answer, which nothing asks for.
pub(crate) fn remember_scan(games: &[Game]) {
    let seen: Vec<_> = games
        .iter()
        .flat_map(|game| &game.targets)
        .map(|target| (target.path.clone(), look(target)))
        .collect();
    SEEN.write().unwrap_or_else(|e| e.into_inner()).extend(seen);
}

/// Runs in a job that read one folder again (its status): what that folder holds now.
pub(crate) fn remember_folder(path: &Path) {
    let target = retoque_discovery::TargetDir {
        path: path.to_owned(),
        executables: Vec::new(),
        score: 0,
        dlss: Vec::new(),
        has_receipt: false,
        proxies_present: retoque_discovery::PROXY_DLLS
            .iter()
            .filter(|name| path.join(name).is_file())
            .map(|name| (*name).to_owned())
            .collect(),
        loaders: Vec::new(),
    };
    let seen = look(&target);
    SEEN.write()
        .unwrap_or_else(|e| e.into_inner())
        .insert(path.to_owned(), seen);
}

/// What the jobs last saw there. A folder no job has read (none in the app: every listed folder comes from a scan)
/// is read here.
fn seen(target: &retoque_discovery::TargetDir) -> Seen {
    let remembered = SEEN
        .read()
        .unwrap_or_else(|e| e.into_inner())
        .get(&target.path)
        .copied();
    remembered.unwrap_or_else(|| look(target))
}

#[derive(Default)]
pub(crate) struct LibraryState {
    pub games: Vec<Game>,
    pub update_all: crate::update_all::State,
    pub art: HashMap<String, Artwork>,
    pub status: HashMap<PathBuf, Status>,
    pub search: String,
    pub installed_only: bool,
    pub migration: crate::folders_ui::FolderMigration,
    /// What uninstalled games left behind, from the last scan; the review modal and its job.
    pub leftovers: Vec<retoque_core::leftovers::Leftover>,
    pub leftovers_review: bool,
    pub leftovers_busy: bool,
    pub leftovers_focus: crate::widgets::FocusRestore,
    /// Set by each completed scan: the home grid opens at its top.
    pub scroll_to_top: bool,
    generation: u64,
    pub derived: DerivedLibrary,
}
#[derive(Default)]
pub(crate) struct DerivedLibrary {
    generation: Option<u64>,
    query: String,
    installed_only: bool,
    lang: Option<Lang>,
    pub installed: Vec<usize>,
    pub others: Vec<usize>,
    pub states: Vec<InstallState>,
    pub details: Vec<String>,
    pub installed_count: usize,
    pub updates: usize,
    pub hidden_non_games: usize,
}

fn state_priority(state: InstallState) -> u8 {
    match state {
        InstallState::Partial => 5,
        InstallState::Update => 4,
        InstallState::Current => 3,
        InstallState::Installed => 2,
        InstallState::Untracked => 1,
        InstallState::Missing => 0,
    }
}

pub(crate) fn single_target_install_state(
    target: &retoque_discovery::TargetDir,
    status_map: &HashMap<PathBuf, Status>,
    payload: Option<&str>,
) -> InstallState {
    let seen = seen(target);
    let is_dlss_root = seen.dlss_receipt;
    if let Some(s) = status_map.get(&target.path)
        && !(is_dlss_root && s.optiscaler_proxies.is_empty())
    {
        let st = install_state(s, payload);
        if st != InstallState::Missing {
            return st;
        }
    }
    if target.has_receipt && !is_dlss_root {
        InstallState::Installed
    } else if seen.retoque {
        InstallState::Untracked
    } else {
        InstallState::Missing
    }
}

pub(crate) fn game_install_state(
    game: &Game,
    selected_target: Option<&PathBuf>,
    status_map: &HashMap<PathBuf, Status>,
    payload: Option<&str>,
) -> InstallState {
    if let Some(sel) = selected_target
        && let Some(t) = game.targets.iter().find(|t| &t.path == sel)
    {
        let st = single_target_install_state(t, status_map, payload);
        if st != InstallState::Missing {
            return st;
        }
    }
    game.targets
        .iter()
        .map(|t| single_target_install_state(t, status_map, payload))
        .max_by_key(|s| state_priority(*s))
        .unwrap_or(InstallState::Missing)
}

impl LibraryState {
    pub fn invalidate(&mut self) {
        self.generation = self.generation.wrapping_add(1);
    }
    pub fn refresh(&mut self, targets: &BTreeMap<String, PathBuf>, payload: Option<&str>, lang: Lang) {
        let d = &mut self.derived;
        if d.generation == Some(self.generation)
            && d.query == self.search
            && d.installed_only == self.installed_only
            && d.lang == Some(lang)
        {
            return;
        }
        d.generation = Some(self.generation);
        d.query.clone_from(&self.search);
        d.installed_only = self.installed_only;
        d.lang = Some(lang);
        d.installed.clear();
        d.others.clear();
        d.states.clear();
        d.details.clear();
        d.installed_count = 0;
        d.updates = 0;
        d.hidden_non_games = 0;
        let query_empty = self.search.trim().is_empty();
        let mut search = crate::fuzzy::Search::new(&self.search);
        let mut scores = vec![0; self.games.len()];
        for (index, game) in self.games.iter().enumerate() {
            let target = targets
                .get(&game.id)
                .filter(|p| game.targets.iter().any(|t| &t.path == *p))
                .or_else(|| game.targets.first().map(|t| &t.path));
            let state = game_install_state(game, target, &self.status, payload);
            let installed = state != InstallState::Missing;
            d.installed_count += usize::from(installed);
            d.updates += usize::from(state == InstallState::Update);
            d.states.push(state);
            d.details.push(format!(
                "{} · {}",
                store_name(lang, game.store),
                runtime_short(lang, &game.runtime)
            ));
            if query_empty && !installed && game.is_non_game() {
                d.hidden_non_games += 1;
                continue;
            }
            let Some(score) = search.score(&game.name) else {
                continue;
            };
            scores[index] = score;
            if installed {
                d.installed.push(index);
            } else if !self.installed_only {
                d.others.push(index);
            }
        }
        let order = |a: &usize, b: &usize| {
            scores[*b]
                .cmp(&scores[*a])
                .then_with(|| self.games[*a].name.cmp(&self.games[*b].name))
        };
        d.installed.sort_by(order);
        d.others.sort_by(order);
    }
    pub fn generation(&self) -> u64 {
        self.generation
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use retoque_discovery::{Runtime, Store};
    fn game(name: &str) -> Game {
        Game {
            id: name.into(),
            name: name.into(),
            store: Store::Manual,
            install_dir: PathBuf::new(),
            runtime: Runtime::Unknown,
            targets: vec![],
            registered: true,
            dlss_anywhere: vec![],
        }
    }
    #[test]
    fn cache_changes_only_on_generation_query_filter_or_language() {
        let mut library = LibraryState {
            games: vec![game("Alpha"), game("Beta")],
            ..Default::default()
        };
        let targets = BTreeMap::new();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.others, [0, 1]);
        let pointer = library.derived.details[0].as_ptr();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.details[0].as_ptr(), pointer);
        library.search = "BETA".into();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.others, [1]);
        library.installed_only = true;
        library.refresh(&targets, None, Lang::En);
        assert!(library.derived.others.is_empty());
        library.installed_only = false;
        library.games[1].name = "Gamma".into();
        library.invalidate();
        library.refresh(&targets, None, Lang::Pt);
        assert!(library.derived.others.is_empty());
    }

    #[test]
    fn non_game_steam_apps_hidden_unless_searched_while_tmodloader_remains_visible() {
        let steam_app = |id: u32, name: &str| Game {
            id: format!("steam:{id}"),
            name: name.into(),
            store: Store::Steam,
            install_dir: PathBuf::from(format!("/synthetic/common/{name}")),
            runtime: Runtime::Proton {
                app_id: id,
                prefix: None,
            },
            targets: vec![],
            registered: true,
            dlss_anywhere: vec![],
        };
        let mut library = LibraryState {
            games: vec![
                steam_app(1001, "Photon Blade"),
                steam_app(1281930, "tModLoader"),
                steam_app(993090, "Lossless Scaling"),
                steam_app(556450, "The Forest Dedicated Server"),
                steam_app(715850, "The Divinity Engine 2"),
            ],
            ..Default::default()
        };
        let targets = BTreeMap::new();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.games.len(), 5, "all 5 apps still count as discovered");
        assert_eq!(library.derived.hidden_non_games, 3);
        assert_eq!(
            library.derived.others,
            vec![0, 1],
            "only Photon Blade and tModLoader should appear in the default grid"
        );

        // Searching reveals hidden non-game tools/servers/editors.
        library.search = "Lossless".into();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.others, vec![2]);

        library.search = "Dedicated Server".into();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.others, vec![3]);

        library.search = "Divinity Engine".into();
        library.refresh(&targets, None, Lang::En);
        assert_eq!(library.derived.others, vec![4]);
    }

    #[test]
    fn the_library_is_projected_from_what_the_jobs_saw_without_reading_game_folders() {
        use retoque_discovery::TargetDir;

        let temp = tempfile::tempdir().unwrap();
        let folder = temp.path().join("Game");
        std::fs::create_dir_all(&folder).unwrap();
        // An old script's install: OptiScaler under a proxy name, no receipt.
        let proxy = folder.join("dxgi.dll");
        std::fs::write(&proxy, b"MZ OptiScaler working as dxgi synthetic").unwrap();
        let target = TargetDir {
            path: folder.clone(),
            executables: vec!["Game.exe".into()],
            score: 10,
            dlss: vec![],
            has_receipt: false,
            proxies_present: vec!["dxgi.dll".into()],
            loaders: vec![],
        };
        let mut found = game("Game");
        found.targets = vec![target.clone()];
        let status = HashMap::new();
        remember_scan(std::slice::from_ref(&found));
        assert_eq!(
            single_target_install_state(&target, &status, None),
            InstallState::Untracked
        );
        // The folder changes behind the app: the projection keeps the scan's answer, it does not open the folder.
        std::fs::remove_file(&proxy).unwrap();
        assert_eq!(
            single_target_install_state(&target, &status, None),
            InstallState::Untracked
        );
        // A job that reads the folder again (its status, after the watcher saw the change) brings the new answer.
        remember_folder(&folder);
        assert_eq!(
            single_target_install_state(&target, &status, None),
            InstallState::Missing
        );
        std::fs::write(&proxy, b"MZ OptiScaler working as dxgi synthetic").unwrap();
        remember_folder(&folder);
        assert_eq!(
            single_target_install_state(&target, &status, None),
            InstallState::Untracked
        );
        // A proxy name another program holds is not Retoque.
        std::fs::write(&proxy, b"MZ ReShade synthetic").unwrap();
        remember_folder(&folder);
        assert_eq!(
            single_target_install_state(&target, &status, None),
            InstallState::Missing
        );
    }

    #[test]
    fn multi_target_game_stays_with_retoque_when_any_target_has_receipt_or_observed_proxy() {
        use retoque_discovery::TargetDir;

        let temp = tempfile::tempdir().unwrap();
        let sparta = temp.path().join("Sparta");
        let win64 = sparta.join("MortalShell2/Binaries/Win64");
        std::fs::create_dir_all(&win64).unwrap();
        std::fs::write(
            win64.join("dxgi.dll"),
            b"MZ OptiScaler working as dxgi synthetic old script",
        )
        .unwrap();

        let game = Game {
            id: "steam:5678".into(),
            name: "Mortal Shell II".into(),
            store: Store::Steam,
            install_dir: sparta.clone(),
            runtime: Runtime::Windows,
            targets: vec![
                TargetDir {
                    path: win64.clone(),
                    executables: vec!["MortalShell2-Win64-Shipping.exe".into()],
                    score: 140,
                    dlss: vec![],
                    has_receipt: false,
                    proxies_present: vec!["dxgi.dll".into()],
                    loaders: vec![],
                },
                TargetDir {
                    path: sparta.clone(),
                    executables: vec!["Sparta.exe".into()],
                    score: 5,
                    dlss: vec![],
                    has_receipt: false,
                    proxies_present: vec![],
                    loaders: vec![],
                },
            ],
            registered: true,
            dlss_anywhere: vec!["nvngx_dlss.dll".into()],
        };

        let mut library = LibraryState {
            games: vec![game],
            ..Default::default()
        };
        // Even if the root target has Status::default() (e.g. after DLSS restore at root) and is in `targets`:
        library.status.insert(sparta.clone(), Status::default());
        library.status.insert(
            win64.clone(),
            Status {
                optiscaler_proxies: vec!["dxgi.dll".into()],
                ..Default::default()
            },
        );
        let mut picked_targets = BTreeMap::new();
        picked_targets.insert("steam:5678".into(), sparta);
        library.refresh(&picked_targets, None, Lang::En);

        assert_eq!(
            library.derived.installed,
            vec![0],
            "game must stay in With Retoque when any target has Retoque"
        );
        assert!(library.derived.others.is_empty());
        assert_eq!(library.derived.states[0], InstallState::Untracked);
        assert_eq!(library.derived.installed_count, 1);
    }
}
