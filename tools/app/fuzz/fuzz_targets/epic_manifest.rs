#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let _ = retoque_discovery::parse_gaming_root(data);
    if let Ok(text) = std::str::from_utf8(data) {
        let _ = retoque_discovery::parse_epic_manifest_item(text);
        let _ = retoque_discovery::parse_heroic_installed_json(text, "legendary");
        let _ = retoque_discovery::parse_heroic_game_config(text, "App");
    }
});
