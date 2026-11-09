#![no_main]
use libfuzzer_sys::fuzz_target;
use retoque_core::ini::Ini;

fuzz_target!(|data: &[u8]| {
    let mut ini = Ini::parse(data);
    assert_eq!(ini.to_bytes(), data);
    let _ = ini.get("DlssNr", "Enabled");
    let _ = ini.set("DlssNr", "Enabled", "true");
    assert_eq!(ini.get("DlssNr", "Enabled"), Some("true"));
    let after = ini.to_bytes();
    assert!(!ini.set("DlssNr", "Enabled", "true"));
    assert_eq!(ini.to_bytes(), after);
});
