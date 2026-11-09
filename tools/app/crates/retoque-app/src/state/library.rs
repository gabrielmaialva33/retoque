//! Generation-cached projections shared by sidebar, keyboard navigation and home cards.
use crate::app::{InstallState, Lang, install_state, runtime_short, store_name};
use retoque_core::Status;
use retoque_discovery::{Artwork, Game};
use std::{
    collections::{BTreeMap, HashMap},
    path::PathBuf,
};

#[derive(Default)]
pub(crate) struct LibraryState {
    pub games: Vec<Game>,
    pub art: HashMap<String, Artwork>,
    pub status: HashMap<PathBuf, Status>,
    pub search: String,
    pub installed_only: bool,
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
        let mut search = crate::fuzzy::Search::new(&self.search);
        let mut scores = vec![0; self.games.len()];
        for (index, game) in self.games.iter().enumerate() {
            let target = targets
                .get(&game.id)
                .filter(|p| game.targets.iter().any(|t| &t.path == *p))
                .or_else(|| game.targets.first().map(|t| &t.path));
            let state = target
                .and_then(|t| self.status.get(t))
                .map(|s| install_state(s, payload))
                .unwrap_or(if game.targets.iter().any(|t| t.has_receipt) {
                    InstallState::Installed
                } else {
                    InstallState::Missing
                });
            let installed = state != InstallState::Missing;
            d.installed_count += usize::from(installed);
            d.updates += usize::from(state == InstallState::Update);
            d.states.push(state);
            d.details.push(format!(
                "{} · {}",
                store_name(lang, game.store),
                runtime_short(lang, &game.runtime)
            ));
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
}
