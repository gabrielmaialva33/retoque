# Full production DLSS-G provider on D3D12

```sh
python3 tests/fg-provider-d3d12/run.py --strict
python3 tests/fg-provider-d3d12/run.py --strict --steady-seconds 15
python3 tests/fg-provider-d3d12/run.py --strict --multiplier 3
python3 tests/fg-provider-d3d12/run.py --strict --multiplier 4
python3 tests/fg-provider-d3d12/run.py --self-test  # host evidence parser and rejection controls only
```

Loads the complete existing `x64/out/OptiScaler.dll` and an actual user-supplied DLSS-G model through
an installed GE-Proton. No game is started, no production source is sliced into the harness, and no
backend is replaced. See [the design](../../docs/dlssnr/design/automated-fg-provider.md).

The synthetic game submits a deterministic moving checker plane, depth, pixel motion and perspective
camera through the public game-style Streamline API. OptiScaler intercepts that input and creates its
own real DLSS-G provider/swapchain. The actual output plugin's GetState is resolved from its loaded
PE exports and its address is checked against the plugin module; the synthetic game-facing GetState
is never accepted as evidence.

Four phases cover initial enable, game disable, re-enable and resize (1280x720 to 1600x900), with
completed GPU/input fences. The quick schedule uses 96/48/96/96 application presents (336 total).
Each active steady phase must have at least 16 more real-plugin presents than application presents;
disabled steady state must report exactly one actual present per application present.
The first 32 frames of each phase allow production's 22-frame activation debounce and plugin warmup.
An explicit `--steady-seconds` extends the three active phases; the disabled phase remains 48 frames.
All remaining plugin statuses must be OK. A successful DLL load, resource tag, dispatch log or
application Present alone cannot pass this suite.

Prerequisites:

- A current complete production DLL, built separately with `./build-local.sh Release` (`--dll` overrides).
- MSVC Wine toolchain and installed GE-Proton (`MSVC_BIN`, `WINEPREFIX`, `--proton`/`RETOQUE_PROTON`).
- A graphical session, compatible NVIDIA hardware/driver and an existing local kit containing
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`.
  Select it with `--kit`/`RETOQUE_FG_KIT`. The runner also discovers complete local test kits.
- An existing `nvngx_dlss.dll` (`--dlss`, or discovered in a local test kit): OptiScaler disables native
  NGX passthrough without the SR asset, including the real frame-generation calls. No SR feature is run.

All staged DLLs, compiler outputs, logs, the dedicated Proton prefix and `result.json` live under
ignored `artifacts/`. Assets are never downloaded or installed into games. Proton automatic upscaler
upgrades are explicitly disabled. The shared compiler-prefix lock and active-build/game guards run
before compiling/launching; no shared wineserver reset is performed.

Missing prerequisites print SKIP with `runtime_coverage=false`; `--strict` converts them to failure.
Once the process is launched, runtime errors always fail. `result.json` records per-phase counters,
staged binary/source hashes (checked unchanged across execution), DLL hash and repository state. The runtime log retains every frame. The host
self-test rejects missing oracle, no surplus, incomplete GPU completion, wrong enable state, zero disabled presents and
missing frame records. A host timeout regression also starts a wrapper with a SIGTERM-ignoring descendant and proves the owned
process group is killed even when its leader exits first. These checks provide no GPU evidence.

## Requested and observed multipliers

`--multiplier {2,3,4}` defaults to x2. The fixture requests one, two, or three generated frames
through both the game-style options and `[DLSSG] InterpolationCount` for OptiScaler's own output.
`OverrideInterpolationCount=auto` is intentional: that is the separate override of a game's options.
The application records its request before startup; the parser rejects missing/conflicting requests.

`AdaMfgUnlock` remains false unless `--ada-mfg-unlock` is supplied explicitly. That flag writes
the existing option only into the staged fixture INI; the existing production feature may then
perform its documented memory patches. No production source or provider file is patched by this
test code, no model is downloaded, and a request does not prove that the unlock was applied.
The provider's reported cap is recorded from the real output plugin, including any configured policies.

The result separates `requested_multiplier`, `requested_generated_frames`, the observed steady
ratios, and the provider-reported generated-frame cap. `effective_configured_generated_frames` is
unknown: the SDK's third GetState argument supplies options and does not read back applied options.
A cap of three permits a request for three generated frames; it does not prove x4 was observed.

## Runtime regression versus counter qualification

The existing surplus/status/fence checks remain the runtime regression gate. A quick default x2 run
may still PASS that regression while `qualification.status` is INCONCLUSIVE. For an explicit x3/x4
request, or any run with `--steady-seconds`, insufficient qualification produces top-level
INCONCLUSIVE with `runtime_coverage=true` and `runtime_regression=PASS`, and exits nonzero. It cannot
become approval of the requested ratio. Missing required identity evidence also prevents qualification PASS.

The SDK defines `numFramesActuallyPresented` as the count since the preceding GetState call.
The harness records monotonic call timestamps and a baseline per phase. Qualification groups
whole consecutive intervals in active steady state into windows spanning at least five seconds,
following the existing moving-window method. Each completed window must contain exactly the
requested integer ratio of plugin presents to application presents, and the steady cap must be
sufficient. No numerical tolerance or phase-wide average hides a degraded window. Partial tails
are reported as excluded; they are not joined across disable/resize boundaries.

`--steady-seconds N` accepts integers from 5 to 120 and is opt-in. Each active phase keeps its
32-frame warmup and runs until both the requested steady duration and at least `floor(N / 5)`
complete windows of five seconds have been observed. It also retains the 96-frame minimum.
Whole GetState intervals form windows; no interval is divided, no tail is interpolated and no
samples are joined across phases. The parser independently recomputes duration and window counts
from monotonic SAMPLE timestamps and checks the REQUEST and four DURATION records.
The runtime timeout is bounded at `240 + 3 * (N + 5)` seconds; the quick path keeps 240 seconds.
A quick run can be too short for qualification. Repeating short runs or concatenating their logs
does not create a valid window; use a fresh explicit-duration run instead.

CPU/system identity is read on the host. The selected DXGI adapter is recorded by the harness,
not inferred from the machine's name or the first device from an unrelated query. Results include
the selected Proton launcher hash, optional version/driver records, INI and harness hashes, and
file hashes for the observed output-plugin/provider module paths when those paths can be mapped.
CPU identity, launcher/module hashes, and nonempty driver/runtime version records with their file
hashes are required for qualification PASS. Unavailable records remain explicit and make
qualification INCONCLUSIVE; no version is filled from a default or inferred from a GPU name.
These hashes describe files read from disk; they are not hashes of mapped or patched memory.
Staged binaries, sources and the launcher must remain byte-identical across execution.
The production INI writer reformats its file and persists three camera flags under XeFG.
The fixture retains the exact launch INI in `artifacts/OptiScaler.launch.ini`, hashes both
versions and requires every explicitly requested setting to remain unchanged. Only those
three added flags, with this scene's expected values, are permitted; other additions,
missing settings, changed multipliers/unlock policy or changed binaries fail provenance.
`staged_assets_unchanged` reports the literal file comparison; `staged_binaries_unchanged`
and `config_provenance` describe the separately verified contracts.

Plugin actual-present counters are not display scanout capture or proof of distinct generated
images. Application presents are frame counts, not base FPS without a corresponding time interval.
No input latency, image quality, GPU frame budget, game integration, Vulkan/D3D11, menu interaction,
or native Windows validation is claimed by this fixture.

## Qualification matrix: pending

This table describes the prepared targets, not completed measurements of this version.

| GPU | Runtime | x2 | x3 | x4 |
|---|---|---|---|---|
| RTX 4090, available locally | Proton | Pending | Pending | Pending |
| RTX 3060, not available in this session | Native Windows | Not run | Not run | Not run |

The result retains a pending hardware matrix and issues no hardware-family approval. This launcher
uses Linux/Proton; it does not provide a native Windows runner. Earlier measurements documented
elsewhere do not qualify this fixture version, a different provider hash, or an untested GPU.

The host-only `test_qualification.py` exercises exact x2/x3/x4 records and rejects x4 requested with
x2 observed, insufficient cap, short/missing timing, missing identity, conflicting requests, broken
counters, and a phase average that hides a degraded window. Duration tests reject missing, duplicate,
malformed or contradictory REQUEST/DURATION records and independently check complete windows.
They also compile the actual portable C++ duration scheduler and hash temporary identity files.
Those tests, including synthetic records naming a GPU, provide no GPU runtime evidence.

The fixture's command cleanup requires default SIGCHLD and exclusive ownership of child reaping.
After TERM it keeps an unreaped leader for its three-second grace, then signals KILL before wait;
an already-reaped leader never authorizes a group signal. The host timeout self-test records its
descendant's PID/start time and uses a pidfd only if that identity is still live, so its finalizer
does not signal a reused PID or a completed descendant. The three sequence tests use controlled
Popen doubles and prohibit real signals; the full `--self-test` still needs native Linux for its
separate actual process-cleanup control.
