use std::collections::BTreeMap;
use std::time::Instant;

use retoque_discovery::{Config, Store, discover};

fn store_label(store: Store) -> &'static str {
    match store {
        Store::Steam => "Steam",
        Store::Heroic => "Heroic",
        Store::Lutris => "Lutris",
        Store::Epic => "Epic",
        Store::Gog => "GOG",
        Store::Xbox => "Xbox",
        Store::Manual => "Manual",
    }
}

fn main() {
    let start = Instant::now();
    let config = Config::default();
    let (games, skipped) = discover(&config);
    let elapsed = start.elapsed();

    let mut counts: BTreeMap<Store, usize> = BTreeMap::new();
    for g in &games {
        *counts.entry(g.store).or_insert(0) += 1;
    }

    println!(
        "Discovered {} game(s) in {:.2} ms",
        games.len(),
        elapsed.as_secs_f64() * 1000.0
    );
    for (store, count) in &counts {
        println!("  {}: {}", store_label(*store), count);
    }
    if !skipped.is_empty() {
        println!("Skipped ({}):", skipped.len());
        for s in &skipped {
            println!("  - {s}");
        }
    }
    println!();

    for game in &games {
        println!(
            "[{}] {} ({}) -> {}",
            store_label(game.store),
            game.name,
            game.id,
            game.install_dir.display()
        );
        println!(
            "  runtime: {:?} registered={} dlss_anywhere={:?}",
            game.runtime, game.registered, game.dlss_anywhere
        );
        if game.targets.is_empty() {
            println!("  targets: (none)");
        } else {
            for (idx, t) in game.targets.iter().enumerate() {
                let rel = t.path.strip_prefix(&game.install_dir).unwrap_or(&t.path).display();
                let rel_str = {
                    let s = rel.to_string();
                    if s.is_empty() { ".".to_string() } else { s }
                };
                println!(
                    "  target #{}: {} (score={}) exes={:?} dlss={:?} receipt={} proxies={:?} loaders={:?}",
                    idx + 1,
                    rel_str,
                    t.score,
                    t.executables,
                    t.dlss,
                    t.has_receipt,
                    t.proxies_present,
                    t.loaders
                );
            }
        }
    }
}
