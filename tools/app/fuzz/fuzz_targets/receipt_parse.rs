#![no_main]
use libfuzzer_sys::fuzz_target;
use retoque_core::receipt::{Receipt, safe_rel};

fuzz_target!(|data: &[u8]| {
    if let Ok(text) = std::str::from_utf8(data) {
        let _ = safe_rel(text);
        if let Ok(receipt) = Receipt::parse(text) {
            let serialized = receipt.to_text();
            let reparsed = Receipt::parse(&serialized).expect("serialized receipt must reparse");
            assert_eq!(receipt, reparsed);
        }
    }
});
