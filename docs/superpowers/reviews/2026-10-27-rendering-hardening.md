# Rendering hardening: verification record

Status: implementation and available verification complete on the isolated branch.
Scope: the first implementation batch approved on 2026-10-27, on `fix/rendering-hardening-20261027`. The primary checkout is preserved; this record describes an isolated development branch.

## Implemented boundaries

### Build and test evidence

The build entry owns the shared compiler-prefix lock before stamps, logs or compilation. Its supervisor owns child processes, preserves failed-attempt logs and uses bounded termination. A busy lock returns 75; watchdog timeout returns 124. Both DLLs must succeed before publication, and ordinary publication errors roll back prior replacements.

Structured test results carry a fresh invocation ID. Missing, stale, malformed, nonterminal or contradictory evidence cannot become PASS. A failing process overrides a claimed PASS. Legacy suites remain explicitly identified as process-exit evidence.

### Vulkan recording lifetime

NR composition reserves from an eighteen-slot pool before recording. Each live recording retains a lease reserving its descriptor slots and constant slices against reuse. Submission completion alone does not retire those references because executable command buffers may be resubmitted. Successful reset/free/pool retirement ends the recording lifetime.

The after-upscale generation waits for old recordings before resize, replacement or hand-back. Lifecycle observation includes application hooks, the private present host, and independent observers when the wDx12 hook bundle is skipped. Device abandonment clears recording metadata while preserving the existing policy against GPU callbacks on an abandoned device.

Registry retirement performs no allocation, including under the allocating-empty-container portability fixture. Bulk free detaches all old entries before invoking a deleter, preserving a handle recreated during reentry.

Design: [Vulkan recording lifetime](../../dlssnr/design/vulkan-recording-lifetime.md).

### D3D12 owner and copy contracts

The first compositor/cache device is retained explicitly for the process session. Command lists, guides, colour, output, optional exposure and the actual Present queue are checked against their relevant owner before incompatible recording. `EnsureCompose` returns an explicit admission result and every caller checks it.

A different device is refused without replacing caches or releasing potentially pending resources. The owner survives the existing partial Shutdown; enabling NR on another device requires a process restart.

Whole-guide copies validate full shape, DXGI type family, plane count and sample count/quality. The incompatible R24G8/R32G8X24-to-R32 fallbacks are removed; legal typed alternatives remain accepted. Present capture keeps its distinct mip-zero/depth-plane-zero contract and invalidates a selected slot if its allocated clone is rejected.

Design: [D3D12 resource ownership](../../dlssnr/design/dx12-resource-ownership.md).

### Provider qualification evidence

The fixture separates requested x2/x3/x4, observed plugin counters, reported capability and qualifying five-second steady windows. The exact launch INI is retained. A production rewrite is admitted only if every explicit value survives unchanged and the only additions are the observed XeFG camera flags. Binary, source and launcher hashes must remain unchanged.

The SDK options parameter is an input, not effective-setting readback; that unavailable value remains null. Ada MFG unlock remains off unless explicitly requested.

## Implementation commits

| Commit | Change |
|---|---|
| `27883af3` | Owned build processes and structured test outcomes |
| `72c9ecd0` | Vulkan recording/resource lifetime |
| `79acf66d` | D3D12 device ownership, guide copies and contract fixtures |
| `2d6d2604` | Frame-generation runtime and qualification evidence |

## Executed evidence

Counts within a suite are not additional independent suites beyond the host aggregate.

| Check | Result | Evidence scope |
|---|---|---|
| Build lifecycle | 26 native cases PASS | Lock, ownership, timeout, signal and output publication |
| Test runner results | 33 native cases PASS | Structured outcomes, stale evidence and owned cleanup |
| Replay-format regression | 9 cases PASS | Importlib sibling-helper fix |
| Vulkan recording helper | 12 groups PASS | Admission, reset, resubmission, reentry and slot leases |
| Vulkan production-body integration | 9 cases per GCC and Clang PASS | Slot immutability, stale batches, discard layouts, generation retirement and abandonment |
| Vulkan wrappers and installation | 12 + 8 groups PASS | Application, private host and independent-observer paths |
| Registry allocation failures | 22 cases PASS | Native containers and allocating-empty-container portability fixture |
| D3D12 guide/owner/capture | 29 native cases PASS | Device references, refusal boundaries, copy compatibility and slot invalidation |
| Before-upscale integration | 70 boundary cases and guide rules PASS | State restoration, refusal before effects, 1,000-iteration allocation/DRS controls |
| D3D12 internal linkage | PASS | 96 original shared objects plus 1 explicit extension; identity across two consumer TUs, negative linkage controls and 89 uniquely owned functions |
| Provider host contracts | 21 tests PASS | Ratios, timing, identity, configuration and cleanup sequencing |
| Complete host aggregate | 90 PASS; all other statuses zero | Final complete execution after the D3D12 structural-contract migration |
| Final Release | PASS, exit 0 | MSVC under Wine; both output DLLs published |
| Real Vulkan NR fixtures | 4 cases PASS on final DLL | `nr`, `nr-route-quiet`, `nr-route-quiet-wdx12`, `nr-route-guides` |
| Real production provider x2 | Runtime regression PASS; qualification INCONCLUSIVE | 336 application frames; enable, disable, re-enable and resize |
| Earlier D3D12 FG input fixture | PASS | 4 DirectXMath cases, 64 GPU readbacks, 3 expected-failure controls; debug layer unavailable |

Native C++ ownership fixtures ran with ASan, UBSan and leak detection. The allocating-container variant models a permitted STL strategy; it is not execution of the MSVC standard library. The D3D12 input fixture ran during the preceding tooling integration, before the final D3D12 cache/copy changes; it is not represented as a final-DLL rerun.

The final host aggregate is `x64/logs/hardening-host-verified-summary.json`, run ID `6ddb1a045f8d41829f08cfeff15b1b47`. All 90 entries remain explicitly labeled process-exit evidence. Inspection of their logs found no leading SKIP, INCONCLUSIVE, TIMEOUT, ABORTED or ZERO COVERAGE markers. Copies of those logs are retained under `x64/logs/hardening-host-verified/`.

## Exact runtime/build snapshot

The final Release was built from the reviewed production sources while HEAD was `27883af3b142a3b6ae52b54c32b4de5952331fbf` with the implementation changes uncommitted. Commits made afterward do not change that embedded build identity. Subsequent edits are test-contract/documentation integration, not new production behavior.

| Output | SHA256 |
|---|---|
| `OptiScaler.dll` | `e9e08f97a940b4b3185fcbf5924a57535277a733a753912263b1c0c4399b7771` |
| `nvngx.dll_dlssnr.dll` | `a7e29f8b631fd08ee2360affdf129fd8ada7abda02358116990d67dd1576e3b8` |

The build retains the preexisting LNK4098 library-conflict warning. Log: `x64/logs/hardening-final-release.log`. Hash receipt: `x64/logs/hardening-final-binary-sha256.txt`.

All four final Vulkan cases and the final provider run used the OptiScaler DLL above. The Vulkan fixture reported zero host validation errors under its attribution rule, with existing explicit `VUID-vkCmdDraw-None-09600` exceptions counted as 2, 6, 0 and 6. Those exceptions remain validation findings, not a claim of globally clean validation. Final hand-back drains ranged from 98 to 1,005 ms; this run is not a performance comparison. Log: `x64/logs/hardening-vulkan-final-runtime.log`.

Provider x2 ran on the observed RTX 4090, GE-Proton11-11 and NVIDIA 615.78.08. Each active steady phase counted 64 application presents and 128 actual plugin presents; the disabled steady phase counted 16 and 16. No active phase contained a complete five-second steady window, so qualification remained INCONCLUSIVE. No Ada unlock was requested. The provenance check passed on this final execution: explicit values are compared case-sensitively, while section/key identity is normalized and only formatting changes plus the three expected XeFG additions are admitted.

Provider aggregate: `x64/logs/hardening-provider-final-summary.json`, run ID `12ebf49004d0414083244451e1c85702`. Its structured suite result has run ID `10cd7a8c6dfc4c07899224381b5e13c6`.

## Integration findings retained

The first provider execution failed its new byte-for-byte INI provenance check. That failure and frame log remain under `x64/logs/`. The production writer reformats the INI and persists three XeFG camera flags; the corrected judge distinguishes that verified rewrite from changed settings or binaries.

An earlier 88-suite host run failed the replay-format importer; the sibling-helper import was fixed and covered. A later 90-suite run passed completely before D3D12 integration. The first complete run after D3D12 changes returned 89 PASS / 1 FAIL because the structural snapshot still froze the old globals and resource bodies. That failure remains in `x64/logs/hardening-host-final-summary.json` (run ID `cbf796812d1d4856bd7dad1b77e5d39b`). The contract migration preserved all 96 original state/initializer contracts and all negative linkage guards, added the new owner explicitly, and passed the complete native linkage/resource check. Nine original resource hashes remain unchanged; only the two intentionally changed helpers and the four new helpers have updated or added entries.

Independent reviews covered process supervision, structured evidence, Vulkan lifecycle/observers/OOM, provider provenance and the final D3D12 guards. A D3D12 review finding about post-allocation capture refusal was reproduced before adding slot invalidation.

## Remaining limits and follow-ups

- No native Windows or RTX 3060 execution, and no x3/x4 hardware qualification.
- Plugin presentation counters do not establish scanout, distinct generated images, image quality, latency or performance gains in a game. Longer steady phases and fresh runtime runs are needed for qualification windows.
- The device-abandonment tests exercise production bodies against controlled boundaries, not an actual live GPU device-loss event.
- The upstream `OS_Vk` descriptor/constant ring is outside this batch's lease pool.
- A preexisting partial meter-readback allocation path remains a separate follow-up.
- D3D12 intentionally requires a process restart to admit a different device.
- Output publication promotes each DLL individually and rolls back ordinary errors/signals. The pair is not atomic for concurrent readers or resilient as a pair transaction to power loss.
