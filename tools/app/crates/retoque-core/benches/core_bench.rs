use std::io::Cursor;
use std::path::Path;

use retoque_core::ini::Ini;
use retoque_core::pe_version::read_version;
use retoque_core::receipt::Receipt;

#[global_allocator]
static ALLOC: divan::AllocProfiler = divan::AllocProfiler::system();

fn main() {
    divan::main();
}

fn shipped_ini_bytes() -> Vec<u8> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../OptiScaler.ini");
    std::fs::read(path).expect("read shipped OptiScaler.ini")
}

fn pe_fixture(pe64: bool) -> Vec<u8> {
    fn put16(b: &mut [u8], n: usize, v: u16) {
        b[n..n + 2].copy_from_slice(&v.to_le_bytes());
    }
    fn put32(b: &mut [u8], n: usize, v: u32) {
        b[n..n + 4].copy_from_slice(&v.to_le_bytes());
    }
    let mut b = vec![0; 1024];
    b[..2].copy_from_slice(b"MZ");
    put32(&mut b, 60, 64);
    b[64..68].copy_from_slice(b"PE\0\0");
    put16(&mut b, 70, 1);
    let ol = if pe64 { 240 } else { 224 };
    put16(&mut b, 84, ol);
    put16(&mut b, 88, if pe64 { 0x20b } else { 0x10b });
    let d = 88 + if pe64 { 112 } else { 96 };
    put32(&mut b, d - 4, 16);
    put32(&mut b, d + 16, 0x1000);
    put32(&mut b, d + 20, 256);
    let s = 88 + ol as usize;
    put32(&mut b, s + 12, 0x1000);
    put32(&mut b, s + 16, 256);
    put32(&mut b, s + 20, 512);
    let r = &mut b[512..];
    for (at, id, to) in [(0, 16, 0x80000020), (32, 1, 0x80000040), (64, 1033, 96)] {
        put16(r, at + 14, 1);
        put32(r, at + 16, id);
        put32(r, at + 20, to);
    }
    put32(r, 96, 0x1080);
    put32(r, 100, 92);
    put16(r, 128, 92);
    put16(r, 130, 52);
    for (i, c) in "VS_VERSION_INFO\0".encode_utf16().enumerate() {
        put16(r, 134 + i * 2, c);
    }
    put32(r, 168, 0xfeef04bd);
    put32(r, 172, 0x10000);
    put32(r, 176, 1 << 16 | 2);
    put32(r, 180, 3 << 16 | 4);
    b
}

#[divan::bench]
fn ini_parse(bencher: divan::Bencher) {
    let bytes = shipped_ini_bytes();
    bencher.bench_local(|| divan::black_box(Ini::parse(divan::black_box(&bytes))));
}

#[divan::bench]
fn ini_get_25_keys(bencher: divan::Bencher) {
    let bytes = shipped_ini_bytes();
    let ini = Ini::parse(&bytes);
    const KEYS: &[(&str, &str)] = &[
        ("DlssNr", "Enabled"),
        ("DlssNr", "Stage"),
        ("DlssNr", "WorkingScale"),
        ("DlssNr", "ModelFile"),
        ("DlssNr", "GameProfiles"),
        ("DlssNr", "HookMethod"),
        ("DlssNr", "ApplyModel"),
        ("DlssNr", "HighlightRecovery"),
        ("DlssNr", "Transfer"),
        ("DlssNr", "Compare"),
        ("DlssNr", "CompareSplit"),
        ("DlssNr", "CompareSwap"),
        ("Upscalers", "Dx11Upscaler"),
        ("Upscalers", "Dx12Upscaler"),
        ("Upscalers", "VulkanUpscaler"),
        ("FrameGen", "Enabled"),
        ("FrameGen", "FGInput"),
        ("FrameGen", "FGOutput"),
        ("FrameGen", "FGNvngxReplacement"),
        ("LSFG", "Enabled"),
        ("LSFG", "FlowScale"),
        ("LSFG", "PerformanceMode"),
        ("LSFG", "ShaderDir"),
        ("LSFG", "DllPath"),
        ("LSFG", "Multiplier"),
    ];
    bencher.bench_local(|| {
        for &(sec, key) in KEYS {
            divan::black_box(ini.get(sec, key));
        }
    });
}

#[divan::bench]
fn ini_set_existing_and_noop(bencher: divan::Bencher) {
    let bytes = shipped_ini_bytes();
    let base = Ini::parse(&bytes);
    bencher.with_inputs(|| base.clone()).bench_local_values(|mut ini| {
        let c1 = ini.set("DlssNr", "Enabled", "true");
        let c2 = ini.set("DlssNr", "Enabled", "true");
        let c3 = ini.set("DlssNr", "WorkingScale", "0.75");
        divan::black_box((c1, c2, c3, ini.to_bytes().len()))
    });
}

#[divan::bench]
fn pe_version_read_pe64(bencher: divan::Bencher) {
    let pe = pe_fixture(true);
    bencher.bench_local(|| {
        let mut cursor = Cursor::new(divan::black_box(pe.as_slice()));
        divan::black_box(read_version(&mut cursor))
    });
}

#[divan::bench]
fn pe_version_read_pe32(bencher: divan::Bencher) {
    let pe = pe_fixture(false);
    bencher.bench_local(|| {
        let mut cursor = Cursor::new(divan::black_box(pe.as_slice()));
        divan::black_box(read_version(&mut cursor))
    });
}

#[divan::bench]
fn receipt_parse_and_to_text(bencher: divan::Bencher) {
    let mut text = String::from(
        "# Retoque deploy receipt\nversion\t1\nstatus\tcomplete\nupdated\t2026-11-09T00:00:00Z\nproxy\tdxgi.dll\n",
    );
    for i in 0..20 {
        text.push_str(&format!("file\t{:064x}\tOptiScaler/support_{i}.dll\n", i + 1));
    }
    text.push_str("backup\tdeadbeef\tdxgi.dll\t.retoque/backup/20261109T000000Z/dxgi.dll\n");
    bencher.bench_local(|| {
        let parsed = Receipt::parse(divan::black_box(&text)).unwrap();
        divan::black_box(parsed.to_text())
    });
}
