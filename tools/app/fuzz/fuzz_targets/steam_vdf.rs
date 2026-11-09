#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(text) = std::str::from_utf8(data) {
        let _ = retoque_discovery::parse_vdf(text);
        let _ = retoque_discovery::parse_library_folders_vdf(text);
        let _ = retoque_discovery::parse_appmanifest_acf(text);
        let _ = retoque_discovery::parse_compat_tool_mapping_vdf(text);
    }
});
