use std::io::Cursor;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

use retoque_data::model_compat::inspect;

#[global_allocator]
static ALLOC: divan::AllocProfiler = divan::AllocProfiler::system();

fn main() {
    divan::main();
}

const STREAM_LEN: usize = 270 * 1024 * 1024;

fn synthetic_elf(sm: u16, abi8: bool) -> [u8; 64] {
    let mut b = [0u8; 64];
    b[..4].copy_from_slice(b"\x7fELF");
    b[4..9].copy_from_slice(&[2, 1, 1, if abi8 { 0x41 } else { 0x33 }, if abi8 { 8 } else { 7 }]);
    b[16..18].copy_from_slice(&2u16.to_le_bytes());
    b[18..20].copy_from_slice(&190u16.to_le_bytes());
    b[20..24].copy_from_slice(&1u32.to_le_bytes());
    let flags = if abi8 { (u32::from(sm) << 8) | 2 } else { u32::from(sm) };
    b[48..52].copy_from_slice(&flags.to_le_bytes());
    b[52..54].copy_from_slice(&64u16.to_le_bytes());
    b
}

fn synthetic_fatbin(sm: u32) -> [u8; 88] {
    let mut b = [0u8; 88];
    b[..8].copy_from_slice(&[0x50, 0xed, 0x55, 0xba, 1, 0, 16, 0]);
    b[8..16].copy_from_slice(&72u64.to_le_bytes());
    b[16..20].copy_from_slice(&[2, 0, 1, 1]);
    b[20..24].copy_from_slice(&64u32.to_le_bytes());
    b[24..32].copy_from_slice(&8u64.to_le_bytes());
    b[44..48].copy_from_slice(&sm.to_le_bytes());
    b
}

fn shared_stream() -> &'static Mutex<Vec<u8>> {
    static BUF: OnceLock<Mutex<Vec<u8>>> = OnceLock::new();
    BUF.get_or_init(|| {
        let mut buf = vec![0xA5u8; STREAM_LEN];
        // Place synthetic ELF and fatbin headers at multiple offsets, including across 64 KiB boundaries.
        for (idx, sm) in [
            (65_530usize, 75u16),
            (32 * 1024 * 1024 + 65_520, 86),
            (128 * 1024 * 1024, 89),
            (240 * 1024 * 1024, 120),
        ] {
            let elf = synthetic_elf(sm, sm >= 100);
            buf[idx..idx + 64].copy_from_slice(&elf);
            let fb = synthetic_fatbin(u32::from(sm));
            buf[idx + 128..idx + 128 + 88].copy_from_slice(&fb);
        }
        Mutex::new(buf)
    })
}

#[divan::bench(sample_count = 3, sample_size = 1)]
fn inspect_synthetic_270mb_uncached(bencher: divan::Bencher) {
    static SEQ: AtomicU64 = AtomicU64::new(1);
    let lock = shared_stream();
    bencher.bench_local(|| {
        let mut guard = lock.lock().unwrap();
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        guard[0..8].copy_from_slice(&n.to_le_bytes());
        let mut cursor = Cursor::new(guard.as_slice());
        let res = inspect(&mut cursor).unwrap();
        assert_eq!(res.archs, &[75, 86, 89, 120]);
        divan::black_box(res)
    });
}

#[divan::bench(sample_count = 3, sample_size = 1)]
fn inspect_synthetic_270mb_cached(bencher: divan::Bencher) {
    let lock = shared_stream();
    {
        let mut guard = lock.lock().unwrap();
        guard[0..8].copy_from_slice(&0u64.to_le_bytes());
        let mut cursor = Cursor::new(guard.as_slice());
        let _ = inspect(&mut cursor).unwrap();
    }
    bencher.bench_local(|| {
        let guard = lock.lock().unwrap();
        let mut cursor = Cursor::new(guard.as_slice());
        let res = inspect(&mut cursor).unwrap();
        assert_eq!(res.archs, &[75, 86, 89, 120]);
        divan::black_box(res)
    });
}
