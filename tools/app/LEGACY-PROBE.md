# Legacy API metadata diagnostic (slice 0)

`retoque legacy-probe` is an explicitly invoked, read-only CLI diagnostic. There is no application view,
background discovery job, persisted setting, target injection, GPU call, capture, helper process or game write.
It implements the metadata step of [the legacy API design](../../docs/dlssnr/design/legacy-apis.md#5-recommendation-and-a-non-visible-slice-0).
The scope idea originated in the NeuRotic comparison; no foreign source, rules, tables or translations were copied.

```sh
retoque legacy-probe --exe '<game executable>' --lang pt-BR
retoque legacy-probe --pid 1234 --runtime windows --mode borderless --output overlay
retoque legacy-probe --exe '<game executable>' --pid 1234 --runtime proton --mode windowed --output in-game --json
retoque legacy-probe --exe '<game executable>' --launcher-exe '<launcher executable>' --log '<existing game log>'
```

Choose one actual game executable or process. Both selectors may be provided to check their association. On Linux,
`/proc/PID/exe` describes the OS process (potentially a Wine host), not automatically its Windows guest. Supply
`--exe` with the actual PE and `--pid` to check whether that selected file is mapped. The OS process architecture,
selected file architecture and optional explicitly named launcher file architecture remain separate. No process
ancestry is followed and no unrelated processes are inspected. Discovery's existing helper/launcher classifier
is reused for the selected filename; classification is a conservative refusal, not proof that every other filename
is a game. An owner-selected edition still needs to be recorded separately before choosing a renderer project.

Only SHA-256 identities and a fixed allowlist of graphics module basenames leave the collector. Arbitrary executable
names, paths, command lines, window titles, raw errors, log lines and memory addresses are never serialized.
`--lang en|pt-BR` (or `RETOQUE_LANG`) chooses readable explanations; JSON has stable enum values/reason keys plus
localized explanations. All new prose lives in the shared data message table. There is no automatic log search:
`--log` must name a regular, unlinked file beside the selected executable, and only its first MiB is read.
Existing log callback lines are historical observations, with **unknown current-process binding**. Module/import
presence never becomes active-renderer evidence, and neither a route key nor a creation line qualifies a game.

## Completeness and refusal

PE machine and ordinary imports share `pe_version`'s bounded COFF parser. PE delay imports and ELF dependencies are
explicitly partial; ELF class/machine/header length are checked without loading the image. Unknown machine,
unreadable/truncated file, access denial, PID reuse/exit, executable mismatch, launcher selection and budget
exhaustion keep a named refusal. A failed identity recheck discards process-derived facts. The process start stamp
is checked before and after inspection; a snapshot is still an observation, not an OS transaction or proof that
file bytes equal a previously mapped image. No process memory is read to hash the loaded code.

`inspection_complete` describes the selected image/hash, import coverage, process identity/association and module
inventory. Window mode stays unknown: geometry alone cannot prove exclusive versus borderless presentation.
On Windows, geometry is reported only for one visible, non-minimized window owned by the selected PID; the window
enumeration queries other owners' PIDs only to discard their handles, never their titles or geometry. Linux does
not query the compositor. `--runtime`, `--mode` and `--output` are clearly marked owner declarations, not observations.
Windows queries `GraphicsCaptureSession::IsSupported` in the diagnostic thread without creating a capture session.
Failure leaves availability unknown; API availability is not game capture qualification.

The matrix explains Windows D3D10/GL, x86, D3D10/DXVK, D3D/wined3d, native Linux GL and existing D3D11/D3D12/Vulkan
routes. Conditional keys are documentation only; the tool never enables them. Native D3D10/GL adapters and x86
in-process NR are unavailable. Windows external capture remains experimental and separate from in-game output.
DXVK implementation/loader reachability cannot be established from a pair of DLL names.

## Bounds and evidence limits

One invocation takes one selected-process module inventory. Windows uses a fixed 1024-handle module buffer;
Linux reads at most 1 MiB of maps and 1024 mapping rows. Window enumeration stops at 1024 handles. Logs are capped
at 1 MiB; PE imports at 96 sections, 256 descriptors and 255 bytes per name. SHA-256 streams files up to 256 MiB
through a 64 KiB buffer; larger or unfinished hashes stay unknown. No file is loaded as executable code.

The CLI has a two-second receive deadline around an in-process worker with a 512 KiB stack. On expiry it emits a
small all-unknown report; CLI process exit ends any stalled worker. The collector also checks deadlines while
hashing/enumerating. This is a wall-time budget subject to OS scheduling and stdout backpressure, not a real-time
scheduler guarantee. No second process is started. Explicit buffers are well below the proposed 32 MiB additional
memory budget, and output is rejected above 64 KiB, including its trailing newline. Native API/library working-set
cost must still be measured on Windows; allocation bounds alone are not that measurement.

Behavior tests live in `retoque-core/src/legacy_probe` and the CLI module. Run from this workspace with the
workstation memory caps:

```sh
cargo test -j 4 -p retoque-core -p retoque-cli legacy_probe
python3 check-legacy-probe-mutants.py
cargo clippy -j 4 -p retoque-core -p retoque-cli -p retoque-data --all-targets -- -D warnings
cargo check -j 4 --target x86_64-pc-windows-gnu -p retoque-core -p retoque-cli
```

The mutation tool temporarily edits only its named local source files, restores them in `finally`, and runs one
focused test at a time. Do not run it alongside edits/builds in this checkout. A compile failure does not count as
a killed mutant. The whole workspace and host tier are separate required integration checks.

Native Windows process permissions, cross-bitness module enumeration, capture availability, window ownership
races and peak working set remain unqualified until measured there. Synthetic tests and cross compilation prove
no game/GPU behavior, image quality, latency, anti-cheat acceptance or compatibility. No real game was invoked
or modified to validate this slice.
