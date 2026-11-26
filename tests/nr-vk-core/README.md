# Vulkan core ownership and storage contracts

Run `python3 tests/nr-vk-core/run.py`. Host only; Python plus g++ and the Vulkan headers.
The production proxy adapter bodies execute with a counted fake core, and the metadata headers
run under ASan/UBSan: borrowed/owned starts, repeated stop,
game restart on the same/new device, unrelated-device stop and interleaved private init publication.
The activity clock covers never-run, one-second expiry, clock reversal, return to native and clear.
Storage admission checks the enabled feature, device API 1.3 and flags2 paths, per-format refusal,
once-only diagnostics, missing query/observation and device reuse. Copied requests cover app-enabled,
Features2 behind another node, legacy features, absent features, unsupported hardware, NR off, internal
devices and unknown chain nodes; application memory and unrelated feature bits must remain intact.
The SPIR-V parser requires Unknown storage formats and the write-only formatless capability in
both committed modules, and checks exact header/binary correspondence.
No NGX DLL, Wine, device, model, GPU, game or native Windows is executed.

The off-path regression executes extracted production create-hook slices with real request/query helpers
and counted fake driver calls, proc lookups, Note calls/logs and registry writes. Both hooks must perform
zero of those with NR off or an internal device; the enabled game path must exercise them. Empty and
populated registries cover unregistered-device destruction; two compiling guard-removal mutants fail.
