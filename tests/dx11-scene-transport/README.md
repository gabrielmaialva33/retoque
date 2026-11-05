# Native D3D11 scene-color transport experiment

Phase 2 of [the scene-stage design](../../docs/dlssnr/design/nr-dx11-scene-stage.md).
This is a fixture, **not a game-facing implementation or a Proton compatibility guarantee**.
No OptiScaler, NGX, user model, production hook, shader or runtime setting is loaded/changed.

```bash
python3 tests/dx11-scene-transport/run.py --host-only
python3 tests/dx11-scene-transport/run.py
python3 tests/run_all.py --only dx11-scene-transport --strict --log-dir tests/dx11-scene-transport/artifacts/runner -v
```

`PROTON_DIR` selects a runtime. Otherwise the guide fixture's installed GE-Proton/Valve discovery
is used. MSVC and its template prefix follow `MSVC_BIN`/`WINEPREFIX`, as the existing guide suite.
The compiler runs in this suite's **private prefix copy**; Proton runs in its own private compat
prefix. Nothing runs against or kills the shared compiler/game prefix. The runner refuses a
concurrent invocation, a detected Steam game, or a shared build/shader compilation in progress.
Missing runtime, compiler, display or required interop capability is **SKIP**, not PASS/FAIL.
A supported sequence with wrong pixels/API failure is FAIL; an unexplained stall is INCONCLUSIVE.
Run alone on the GPU. The registry serializes Wine suites; it cannot coordinate other agent sessions.

## Experiment and oracle

The implementation plan is deliberately confined to this directory: `run.py` owns prerequisites,
private-prefix lifecycle, watchdog cleanup, identity and structured results; `harness.cpp` owns
real API operations and pixel/ordering observations; `test_runner.py` tests evidence classification.
`pch.h` is a fixture-local header. Registry/catalog and the design note document the experiment.

Twenty-four frames at 61x37 RGBA8 UNORM use four reusable transport slots. Every frame has a
spatial and frame-index byte pattern. D3D11 writes an ordinary producer texture and copies it
into an owned shared input. D3D12 dispatches a fixture-only compute shader producing `1 - input`
into a separate shared output. D3D11 consumes that output into an ordinary D3D11 texture and
copies it to a per-frame staging snapshot. Every byte, including alpha and pitched rows, must
match the current frame's expected inversion. No old-frame result is accepted.

The tiny HLSL string is compiled in the fixture with D3DCompile; it is not a copied model shader
or production runtime-compilation path. Lists/allocators are unique per submission, never reset
in flight. All resources stay alive until process exit; failure/timeout paths do not destroy live
slots. Four shared single-writer fences express the order:

```text
Q11: produce/copy -> Signal P
Qnr: Wait P -> invert -> COMMON -> Signal N
Q11: Wait N -> consume/snapshot -> Signal C
Qpresent (when used): Wait C -> copy result to backbuffer -> COMMON/PRESENT -> Signal B
Q11: before slot reuse, Wait its previous B (Present cases)
```

There is **no CPU completion check, event wait, blocking Map or polling loop in the frame submission
loop**. D3D11 Signal/Flush and Present can still stall internally; their CPU call blocks are measured.
After the whole batch is submitted, an observer polls terminal C/B and maps snapshots with
`D3D11_MAP_FLAG_DO_NOT_WAIT`. This final inspection cannot repair an absent transport dependency.
The 12-second observer watchdog records P/N/C/B and the last CPU phase, then exits the child;
it never signals a GPU fence. A 60-second parent timeout covers setup or stuck process exit.
Cleanup kills only this suite's private Wine servers/process group.

| Case | Meaning |
|---|---|
| `probe` | D3D11/D3D12 fence export/import; legacy and `SHARED | SHARED_NTHANDLE` resource opening; keyed texture, zero-timeout D3D11 acquire/release, D3D12 mutex QI. Creation/opening is not a content test. |
| `no-present` | Same-frame producer/compute/consumer chain, without a swapchain. |
| `present` | Chain followed by a real DXGI D3D12 swapchain Present on another logical queue. |
| `present-ahead` | Enqueue the Present queue's future-C wait/copy/signal **before** submitting the work that produces C on the other logical queue. Exercises a potential aliasing hazard. |
| `forced-alias` | Negative control: intentionally put Present and NR on the **same D3D12 queue**. A known cycle prevents N/C/B while P advances. Accepted only with the watchdog's exact fence signature; arbitrary timeout does not pass. |
| `mutant-copy` | Negative control: D3D12 copies instead of inverting. Must complete all frames and reach the pixel oracle, which must report mismatches. Device/setup failure does not pass. |

Read-only `ID3D12DXVKInteropDevice::GetVulkanQueueInfo` queries report whether the two normal
D3D12 queues share a VkQueue and their families. The fixture uses the ABI prefix already declared
in `OptiScaler/misc/IdentifyGpu.h`, without its production dependencies. No native Vulkan submissions
or queue-lock operations are performed. Logical-queue aliasing in the negative control is not
misreported as the installed runtime's mapping of separately created queues.

Keyed mutex support is probed, **not used as an unproved D3D12 synchronization fallback**. A
D3D11 interface and successful D3D12 resource opening do not supply a D3D12 acquire/release protocol.
The demonstrated route, if successful, uses shared fences.

## Evidence and limits

`artifacts/result.json` uses the repository's invocation-bound suite-result schema. Each case has
`trace.log`, `case.json`, DXVK logs and `runtime.log`. The aggregate includes runtime identities,
installed runtime DLL hashes, the exact fixture executable hash, counters, outcomes and timings.
Fresh case evidence is required; zero pixel coverage cannot pass. A skip survives the registry's
result contract. The forced-cycle and wrong-pixel controls are labeled as controls in the evidence;
their raw STALL/FAIL records remain available even when their expected detection passes.

The fixture checks correct D3D11 consumption before Present; it does not capture scanout or prove
that the compositor displayed every submitted frame. It does not exercise native DLSS, NR model
internal queues, HDR, depth/motion formats, dynamic resolution, device loss, a game's FG/swapchain
bridge, all queue topologies or native Windows. Small RGBA8 frames are an ordering test, not a cost
estimate for NR. A successful run does not lift the design's product gate.

The dated runtime result and command/hash receipt belong in the design note and sibling worker
report. Raw artifacts and private prefixes are ignored and must not be committed.

## Phase 3 production transport cases

The same executable now includes `retoque/dlssnr/DlssNr_Dx11SceneTransport.h`, with no production
DLL, NGX/model or game hooks. `product-roundtrip` calls `Transport::Init/Run/Retire`, with an in-place
inversion callback in place of NR, for 24 frames. It queues Present's future consumer dependency
**before** each handoff, observes the returned D3D11 output as later effects would, and checks every
byte. Unlike the original four-slot experiment, production uses one bounded generation and skips
busy admission using completion polling. The fixture polls readiness before offering another frame;
these polls do not satisfy any GPU dependency. Successful retirement is tested after completion.

`product-alias` supplies the Present queue as NR and must reject `SharedVkQueue` before submission.
`product-missing` supplies no Present queue and must reject `QueueIdentityUnavailable`.
`product-mutant` omits compute; all 24 frames must finish and fail the pixel oracle. These exercise the
actual shared resource/fence code and DXVK `GetSubmissionQueue` plus vkd3d `GetVulkanQueueInfo` checks.
They do not exercise the NGX compositor, its submission hook installer or the native DLSS backend seam;
those still require product/game qualification. Unsupported production primitives are SKIP, not PASS.
