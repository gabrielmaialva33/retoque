# Character inspection and regional NR: slice 0 implemented, the rest under review

Date: 2026-11-19; slice 0 added 2026-11-20. **Slice 0 (section 4) is implemented: `[DlssNr] MaskInspection=log`,
log only, off by default, host-tested, not yet built into a DLL or run in a game.** Everything after it is design
under review with no implementation or runtime qualification. The owner reopened character inspection and object
rules on 2026-11-19. The recommendation is to make existing protection understandable first, then require measured
evidence before adding regional image changes. No other key named below exists.
This note does not authorize a detector, dependencies, model acquisition or changes to defaults.

## 1. Evidence and the reference implementation

Retoque source: `8d2d8a1e`, clean at inspection. NeuRotic reference: `beae4eeaab0032b665f7a52684db3aa7345bef69`,
clean at inspection, read only. `N:` paths below are relative to that reference, not files to import into Retoque;
`R:` paths are relative to this repository. Line anchors refer to those snapshots. The idea of a character
inspection surface and saved object rules comes from NeuRotic. No source, tables, translations or model data
were copied. This is a source audit, not a run of its installer, detector or GPU paths.

### Process, capture and visible output

| Source observation | Evidence in NeuRotic |
|---|---|
| Explicitly enabled, off by default; separate installed worker component rather than a complete source-only runtime | `N:tools/nr_semantic_worker/README.md:3-9`; `N:OptiScaler/nr/semantic/character/CharacterInspectorSettings.h:9-32` |
| Windows host creates a same-user, remote-client-rejecting named pipe, verifies installed files, launches isolated embedded `python.exe -I -B`, and passes only the selected handles to the child | `N:OptiScaler/nr/semantic/character/CharacterRuntime.cpp:125-158` |
| Host sends image bytes as well as metadata. Reply is checked against nonce, child PID, image hash, frame sequence, dimensions and epoch; no remote inference is implemented in this worker | `N:OptiScaler/nr/semantic/character/CharacterRuntime.cpp:225-240`; `N:tools/nr_semantic_worker/server.py:9-53` |
| D3D12 thumbnail compute and GPU-to-CPU copy use three slots, with longest image side capped at 960; readback polls completion and copies the newest completed result | `N:OptiScaler/nr/semantic/character/CharacterCaptureDx12.h:18-35,62-70`; `N:OptiScaler/nr/semantic/character/CharacterCaptureDx12.cpp:80-145` |
| D3D11 and Vulkan capture implementations exist. Vulkan records a blit/copy and polls fences, accepts specified SDR/PQ/scRGB formats, and checks format capabilities | `N:OptiScaler/nr/semantic/character/CharacterCaptureDx11.cpp`; `N:OptiScaler/nr/semantic/character/CharacterCaptureVk.cpp:30-47,70-90,128-142` |
| The apparent pre-upscale capture implementation is gated off by a compile-time constant in this snapshot | `N:OptiScaler/nr/semantic/character/CharacterRuntime.h:68`; `N:OptiScaler/nr/semantic/character/CharacterRuntime.cpp:518` |
| Inference is OpenCV DNN on CPU. One detector job and one optional pose job can run concurrently; each has one executor worker, and jobs are not queued behind an active model call. OpenCV is configured for one internal thread | `N:tools/nr_semantic_worker/providers/zoo_character.py:93-131`; `N:tools/nr_semantic_worker/tracking_engine.py:7-49` |
| Newer images receive optical-flow tracking between detector corrections. This is image tracking, not game object identity; pose work is throttled to at least 150 ms between submissions | `N:tools/nr_semantic_worker/tracking.py:31-100`; `N:tools/nr_semantic_worker/tracking_engine.py:25-47` |
| ImGui draws rectangles, class/rule labels, track IDs and scores, plus an optional estimated torso band from pose landmarks. It does not draw a segmentation silhouette | `N:OptiScaler/nr/semantic/character/CharacterInspectorDraw.h:21-86` |

Defaults are four instances, eight labels, 33 ms update interval, 80 ms hold, object detection enabled and torso
estimation disabled (`N:OptiScaler/nr/semantic/character/CharacterInspectorSettings.h:9-15`). These are requested
limits, not achieved throughput. On a qualified real-image boundary the capture path can submit each real frame;
the timer for other boundaries caps the requested interval at 50 ms
(`N:OptiScaler/nr/semantic/character/CharacterRuntime.cpp:419-429`). Thus “one inference every 33 ms” is inaccurate.

The simple snapshot admission has a 150 ms age limit (`N:OptiScaler/nr/semantic/character/CharacterObservation.h:130-140`),
but the live overlay also has a **held-snapshot path**: arrival may be up to 500 ms after capture, with configured
receipt-based hold; motion tracks additionally require image age at most 100 ms without FG and confirmation age
at most 350 ms. Qualified FG can extend the motion-image limit to three real-frame intervals, capped at about
250 ms. Epoch/future/malformed/unqualified-FG checks still apply. Prediction is capped at 20 ms and disabled
when FG is active. See `N:OptiScaler/nr/semantic/character/CharacterBoxHold.h:4-45,107-115` and
`N:OptiScaler/nr/semantic/character/CharacterInspectorOverlay.h:54-64`. Thus “all boxes are less than 150 ms old”
would be false. Freshness checks do not prove the boundary matches the current character; hold/prediction is
particularly unsuitable as automatic permission to edit pixels.

### Models, licences and packaging

The source lock names three ONNX artifacts, all declared Apache-2.0, hash/size pinned and `bundled: false`
in the source manifest (`N:tools/nr_semantic_worker/models.lock.json:4-35`):

- `person_detection_mediapipe_2023mar.onnx`: person detection, 11,990,159 bytes.
- `pose_estimation_mediapipe_2023mar.onnx`: optional per-person landmarks, 5,557,238 bytes.
- `object_detection_nanodet_2022nov.onnx`: fixed object classes, 3,800,954 bytes.

Those are artifact sizes, **not** resident RAM, package size or execution cost. The source worker constructs
its provider from the locked artifacts; acquisition is an explicit preparation step, not an inference download.
Its provider checks sizes and hashes (`N:tools/nr_semantic_worker/server.py:18-23`;
`N:tools/nr_semantic_worker/providers/zoo_character.py:95-100`). The README requires separately assembled Python,
NumPy, OpenCV and model dependencies plus notices. An older packager lists Python 3.12, NumPy 2.3.5 and OpenCV
4.13.0.92 (`N:tools/character-inspector/package_live.py:77-92`); that script alone does not establish the contents
of a current distributed installer.

The pinned OpenCV Zoo revision is `47534e27c9851bb1128ccc0102f1145e27f23f98`. Its own directory READMEs confirm
Apache-2.0 for the files in the [person detector directory](https://raw.githubusercontent.com/opencv/opencv_zoo/47534e27c9851bb1128ccc0102f1145e27f23f98/models/person_detection_mediapipe/README.md),
[pose directory](https://raw.githubusercontent.com/opencv/opencv_zoo/47534e27c9851bb1128ccc0102f1145e27f23f98/models/pose_estimation_mediapipe/README.md)
and [NanoDet directory](https://raw.githubusercontent.com/opencv/opencv_zoo/47534e27c9851bb1128ccc0102f1145e27f23f98/models/object_detection_nanodet/README.md)
(checked 2026-11-19). The upstream pose model has a segmentation output, but NeuRotic's adapter deliberately
decodes landmarks without mask postprocessing (`N:tools/nr_semantic_worker/providers/zoo_character.py:61-80`).
Having that model installed is not evidence that the game receives usable segmentation.

NeuRotic's overall project notice is GPLv3; its draw header identifies an MIT reference and the adapted provider
identifies Apache-2.0 (`N:THIRD_PARTY_NOTICES.md:3,33-37`;
`N:OptiScaler/nr/semantic/character/CharacterInspectorDraw.h:1-3`;
`N:tools/nr_semantic_worker/providers/zoo_character.py:1-5`). A future independent detector package would need an
artifact-level inventory of runtime, wheel, native library, source and model notices; a permissive model notice
does not cover every dependency. No such package or redistribution decision is made here. NVIDIA NR remains
user supplied under Retoque's existing rule; no detector licence grants rights to ship that DLL.

### What object rules actually change

Rules are implemented as a validated saved profile and overlay policy, not per-object model weights:

1. Matching resolves English fixed class names and the Human alias against NanoDet's class vocabulary, with an
   eight-active-class limit. Arbitrary text does not train a detector. Unsupported entries can remain stored.
   See `N:OptiScaler/nr/semantic/object_rules/ObjectRules.h:83-90,100-114` and
   `N:OptiScaler/nr/semantic/object_rules/ObjectRuleEditor.h:109-113`.
2. Eligible detections are filtered by class, score and rule epoch; priority resolves the winning overlay style.
   The rule changes box/label visibility, colours, thickness, opacity and label text, and permits selecting a rule
   from its box. See `N:OptiScaler/nr/semantic/character/CharacterInspectorDraw.h:40-71`.
3. The editor explicitly leaves regional appearance and NR blend/model overrides unavailable. It retains saved
   requests inactive because qualified silhouettes and matching original/NR images are missing. The profile's
   `nr.strategy` and `nr.blend_amount` fields are storage, not a runtime NR consumer. See
   `N:OptiScaler/nr/semantic/object_rules/ObjectRuleEditor.h:104,112-113` and
   `N:OptiScaler/nr/semantic/object_rules/ObjectRules.h:134`.

A tracked-source search of `Rules::GameStore`, `Rules::Eligible`, `Rules::Activity`, `blend_amount` and
`requested_controls` under `OptiScaler/`, excluding generated localization, found configuration/editor/overlay
consumers and the schema, not a renderer applying regional NR weights. Direct inspection of those consumers
agrees with the editor's stated limit. **Confirmed: overlay rules; not confirmed: any current per-object NR effect.**
Regional blend/tint/exposure/model overrides are future capability placeholders here, not a completed advantage
that Retoque must duplicate. No release promise or schedule for them was established from this source checkout.

### Cost and limits: what is known

CPU work includes colour conversion, hashing, pipe transfer, CPU inference, grayscale flow tracking and optional
pose; GPU work includes thumbnail conversion/copy plus overlay geometry. D3D12 owns three readback/thumbnail
slots; tracking keeps up to 24 downscaled images (`N:tools/nr_semantic_worker/tracking.py:86-99`). For illustration,
a tightly packed 960x540 RGBA thumbnail is 2,073,600 bytes; sending 30/s is about 62.2 MB/s **before** padding,
additional copies, conversion or allocator overhead. This is arithmetic, not a throughput measurement.

The runtime records request round-trip time and capture-to-reply age separately; its panel exposes detector,
tracking and pose timing (`N:OptiScaler/nr/semantic/character/CharacterRuntime.cpp:227-240`;
`N:OptiScaler/nr/semantic/character/CharacterInspectorMenu.h:117-122`). The field called inference time includes
host transfer and reply processing, not only the neural network. No comparable game FPS, CPU p95, GPU p95, RAM
peak or detection-quality measurement was established in this audit. None is inferred from the defaults.

Its own source marks the feature experimental, warns that frame-path availability does not certify image quality
or GPU support, reports late samples, and states that silhouettes/regional effects are unavailable (anchors above).
Fixed COCO classes do not supply face identity, armour/material segmentation or game entity IDs. A box can include
background or miss hands/hair; translucent HUD can be inside it. This is acceptable as a labelled diagnostic,
but applying its strength to a rectangular region can produce bright rims, moving patches and wrong-person edits.

## 2. Player value and what Retoque already covers

| Judgeable player request | Existing mechanism and its limit | Remaining useful work |
|---|---|---|
| Keep skin structure gentler than the scene in a close-up | `AutoMask=true` and `SkinStructure=-1` defaults; -1 follows LocalStructure. With auto skin masking active, a nonnegative SkinStructure controls skin separately. Menu disables it without AutoMask. `R:OptiScaler/Config.h:408-412`; `R:retoque/dlssnr/DlssNr_Menu.cpp:2258-2274` | First compare those settings on the same close-up. No second skin-strength slider. This does not promise to preserve face identity or stop every colour/tone edit |
| Leave the whole character, including clothes, closer to the original while relighting the scene | AutoMask is skin-specific; it supplies no exposed character silhouette. Scalar skin controls are passed into the model at create/evaluate; no skin-mask output is exposed by the integration. `R:retoque/dlssnr/DlssNr_DirectRuntime.h:37-50`; `.cpp:565-568,625-626` | A demonstrated gap, requiring same-frame segmentation and bounded regional composition; detector rectangles alone cannot satisfy it |
| Understand why text or a static character edge is protected | D3D12 present `UiProtection` optionally generates an RGBA UI alpha at model working size and passes it with matching Backbuffer. `R:retoque/shaders/dlssnr/DlssNr_Dx12.cpp:2767-2792`; `precompile/dlssnr_uimask.hlsl:13-18,84-90` | Inspection of the actual UI mask, not a “person mask”. Do not enable UiProtection merely to make an inspector view available. No equivalent UI-mask dispatch was found in `DlssNrFeature_Vk.cpp` at this snapshot |
| Understand why a lamp/bright face resists the edit | Composition already has luminance-dependent guard weights, near-black hue trust and an optional highlight-recovery mask. `R:retoque/shaders/dlssnr/precompile/dlssnr.hlsl:1157-1220,1277-1301` | Explain/visualize their actual values, labelled by purpose, without calling them semantic detection |
| Lower one named NPC's strength without changing another | No stable entity-ID or semantic silhouette source is established here; image tracks can switch identities | Defer identity-based rules. A supported “all valid character pixels” rule would itself require a new measured mask provider |

Read [HUD protection](hud-protection.md), including its later macro fix and earlier false rim observation;
[highlight recovery](hdr-highlight-recovery.md); and [residual limit](residual-limit.md). The rim is a concrete
reason why false protection is not visually neutral even when it restores original pixels. Historical runs in
those notes do not qualify today's shader. Composition weights are often shader-local scalars, not separately
allocated textures waiting to be displayed. Existing debug views already show proxy/model/edit
(`R:retoque/shaders/dlssnr/precompile/dlssnr.hlsl:931-970`); reuse their inspection concept instead of duplicating it.

Depth distinguishes surfaces, motion describes movement, and Streamline tags declare buffer roles. None means
“character”. The current [tag inventory](sl-tag-inventory.md) records lifecycle/shape metadata only; it neither
validates contents/queue ownership nor supplies HUD-less/UI-alpha composition. The model's own skin mask is
internal; it can inform the player through existing controls, but is **not an available mask texture** for option B
or an overlay. Absence was checked in the runtime interfaces, model parameter setters and shared resolve shader.

## 3. Options and their tradeoffs

Costs below are expected work, not timings. Linux/Proton and native Windows need independent qualification.

### A. Separate detector process

- **Needs:** qualified image capture, colour conversion, fenced readback, authenticated bounded IPC, worker
  lifecycle, pinned runtime/models and an independently written adapter. Windows can use a packaged Win32 worker;
  under Proton, Windows Python in the prefix is a distinct test target. A native Linux worker would need a new
  transport and packaging contract, not an assumed consequence of Python portability. Native Vulkan capture is
  also a distinct adapter. No online downloader in the game path.
- **Cost/latency:** GPU thumbnail/copy/overlay, CPU copies/inference/tracking, additional RAM and bandwidth. The
  result belongs to the captured frame, often several real frames behind presentation. Never block Present for a
  result. Drop work under pressure. A GPU inference backend would compete with NR and requires its own comparison.
- **Failure:** missing package, slow/crashed worker, wrong colour interpretation, stale or switched tracks, cuts,
  occlusion, stylized people, tiny faces, HUD inside boxes. A false box changes innocent background if used as a
  mask. Detector confidence is not silhouette accuracy. FG output is not an independent new real frame.
- **Privacy/licences:** pixels leave the game process even with local-only pipes. Require explicit opt-in, bounded
  volatile storage, no automatic dumps or network, and complete dependency/model notices. No reuse of NeuRotic
  code or package. Its source declarations are insufficient to certify an installer we did not inspect.
- **Host tests:** synthetic image/reply fixtures, malformed/oversized messages, hash/nonce mismatch, timeouts,
  cancellation, generation changes, newest-only queue and zero launch/capture when off. Host fake-provider success
  cannot prove detector quality, Wine IPC compatibility or GPU readback correctness.

### B. Derive a mask from existing frame signals

- **Needs:** route-qualified depth/motion/UI tags with extent, crop, scale, jitter, owner, real-frame identity,
  queue state and completion provenance. The current tag inventory alone is insufficient. Depth/motion can help
  boundaries and transport; they cannot independently identify skin or a character. The internal skin mask would
  require a documented, validated export that this integration currently lacks. Missing signals mean unavailable.
- **Windows/Proton:** reuse the route's native D3D12 resources, including a qualified D3D11 bridge where applicable;
  Vulkan needs its own resource/lifetime adapter. No cross-API pointer borrowing or new capture hook by implication.
- **Cost/latency:** in-process GPU reads and potentially a compute pass/temporary mask; no Python inference. CPU
  admission is small but not free. Same-frame masks can avoid asynchronous detector latency, while temporal masks
  add history and disocclusion risk. Measuring and rejecting mismatches is more important than their average age.
- **Failure:** wrong depth candidate, moving camera mistaken for moving character, reversed depth, invalid tag
  lifetime, ghosting at occlusion or HUD mismatch. Reject to existing NR output; never fabricate semantic confidence.
- **Privacy/licences/tests:** no pixels need leave the process; keep existing attributions and user-model boundary.
  Test admission against synthetic tags/transforms, NaNs, dynamic resolution, delayed frames, gaps and device loss;
  test mask math on fixtures. Shader/GPU/native Windows acceptance remains additional work.

### C. Inspect existing masks and composition weights only — recommended direction

- **Needs:** an honest availability/status view first; later, opt-in views of the actual UI-alpha texture and
  composition weights. No new person detection. UI labels distinguish “skin control enabled, mask not exposed”,
  “UI mask unavailable here” and an actual observed mask. No material/character-rule editor without a provider.
- **Windows/Proton:** start with metadata from the D3D12 NR path shared by both. Vulkan reports unsupported until
  separately wired; a D3D11 bridge is covered only at the existing D3D12 NR boundary. An overlay needs validated
  frame-to-display mapping and HDR handling; a compiled shader does not certify either platform.
- **Cost/latency:** first slice has bounded CPU bookkeeping/logging, no added GPU work or readback. A later mask
  overlay needs sampled reads and possibly an extra render target; shader-local weights need instrumentation.
  Observe the same evaluated real frame, not a retained texture after a route change. No history solely for drawing.
- **Failure:** wrong-frame display, misleading colour legend, unavailable mask presented as empty, diagnostic pixels
  fed back to the model, or an instrumentation pass changing resource order. The view is explanatory, not a quality
  verdict. Hide invalid content with a named reason rather than freeze a plausible mask.
- **Privacy/licences/tests:** keep pixels on the GPU; only allowlisted status metadata reaches existing logs. No new
  model/dependency/licence burden. Host tests prove status/off/availability and config contracts; later GPU tests
  must prove visualizations match the actual scalar/texture and do not contaminate NR input/history.

### D. Keep today's controls and add no inspector

- **Needs/platforms:** nothing new on Windows or Proton. Existing skin controls, comparison/debug views and
  documentation remain available on their current routes.
- **Cost/latency/privacy/licences:** zero incremental work, frame latency, frame export or dependency burden.
- **Failure/tradeoff:** leaves users guessing why a protection applies and cannot meet whole-character strength
  requests. It avoids detector errors but does not fix existing artifacts.
- **Host tests:** existing regression suites; no invented semantic success criterion. This is the fallback if C
  cannot help users make a better settings choice or if the owner only wants visible person boxes.

## 4. Recommendation and first observational slice

Choose **C first**. It serves the settings decision before spending CPU and installation complexity on a detector
whose current reference does not change regional NR. Consider **B later only for an evidenced missing use case**.
Keep A as an optional research route if same-frame silhouette evidence cannot be obtained otherwise. Do not
market the first slice as character recognition. These are author recommendations, not accepted owner decisions.

### Slice 0: protection status in logs, no pixels changed

**Implemented 2026-11-20** in `retoque/dlssnr/DlssNr_MaskInspection.h`, published from
`DlssNr_Dx12::Dispatch`; host contracts in [nr-mask-inspection](../../../tests/nr-mask-inspection/README.md). Not
built into a DLL and not run in a game: what a game run still has to show is listed at the end of this section.

One key under `[DlssNr]`: **`MaskInspection=off|log`**, default `off`; `auto`/unset resolves to off.
Unknown or malformed values resolve to off, with only the existing config diagnostic (the `DlssNr.MaskInspection:`
entry every non-auto key gets), not a per-frame warning. The requested value is distinct from the effective state:
the text asked for is what is saved, never what it resolved to. No overlay, threshold, detector, class list,
confidence slider, capture directory or worker-path key in this slice, and no menu control.

It round-trips through declaration/default in `OptiScaler/Config.h`, reading and saving in `OptiScaler/Config.cpp`,
and a documented entry in the shipped `OptiScaler.ini` and in Config.md. The suite runs unset, auto, off, log,
malformed and save/reload through the production statements, with unrelated keys kept and a session-only value never
saved as the user's. Nothing but the gate reads the key, so it cannot reset or rebuild the model or change
cadence or pass settings.

The producer is the existing D3D12 NR dispatch boundary on the present route (`d3d12-present`), after its ordinary
UI-alpha decision. One fixed-size value snapshot per Dispatch, on its stack: route, the present's sequence and the
observer generation (numeric local tokens only), the model feature's generation, working extent, passes evaluated of
passes requested, AutoMask and SkinStructure of each evaluated pass, UI alpha handed or not handed with a reason, and
which composition branches the resolve was sent with. No resource pointer is kept. A line states CPU recording facts
as **recorded**, not GPU-completed or visually effective, reports the model's skin mask as internal and not exposed,
and infers no pixel coverage from a setting or a non-null pointer.

The line is said through the log the pass already writes its own diagnostics to, when what it states changed, at
most once per two seconds; the sequence is not part of what makes a line new and each line carries the frame it was
recorded on. No pixel histograms, extra queries, CPU texture map, logger or file of its own, event queue, paths,
labels, image hashes, game identities or capture dumps. A log request on a route with no producer reports unavailable
once through the same bounded path; no hook is installed to find a substitute source.

**Off contract:** at startup off, or with NR disabled, no inspector allocation, process, thread, file, readback,
GPU command, extra tag hook or model query; no change to the existing pass inputs, outputs, constants, resets,
submission order or completion tracking. The gate precedes bookkeeping: off is the config test and one bool of the
observer, read under the lock Dispatch already holds. Existing logging and the tag inventory continue only under
their own rules. Switching off stops publication and ends the generation, so nothing observed before can be said
after; log text already written is not deleted. Re-enabling starts a new observer generation. Resize, route or pass
change, a model rebuild (which a device change forces) and NR having been disabled each start a generation before
any later observation is considered. The observer never releases or takes ownership of GPU resources. Metadata that
cannot be trusted skips the sample.

#### Choices made where this section left room (2026-11-20, author recommendations taken; owner review pending)

- **Where the line is said.** At the end of the same Dispatch that recorded the frame, under the pass's own lock,
  through `LOG_INFO`, like the composition and chain lines beside it. The alternative was the two-second status
  line at Present; it runs on another thread and only on some routes, so it would have needed a lock of its own and
  could have said a snapshot older than the frame it was said on. No lock, thread or queue is added, even with the
  key on.
- **What "why was this region changed or not" needs.** Beyond the fields listed above the line also states the
  residual limit and the final darkening floor (the other two bounded branches of the resolve), whether the model's
  edit reaches the picture at all (`ApplyModel` off, strength zero, a debug view, compare) and whether the frame
  was a model frame or a carried one. Coverage is never stated, and the line ends by saying so.
- **A declined frame.** "Nothing recorded for this frame" is a status of its own, never the previous frame's. A
  decline between two recordings (a second Dispatch refused at the same present) is not said while a recording is
  less than two seconds old; one that lasts is said. Without that rule a game that calls the pass twice per present
  would alternate two lines forever.
- **Model cadence.** Model frames and carried frames alternate by design, so which of the two a line saw is
  provenance, like the frame number, not a change.
- **Total volume.** The two-second rule bounds the rate; 256 lines per process bounds the total, and the last line
  says it is the last. A line is at most 2 KB and is built from integers only (settings at two decimals).
- **Routes.** `d3d12-present` only, as recommended in section 6 (5). The D3D11 bridge, before-upscale, after-RR and
  after-upscale pass through the same Dispatch and say "unavailable on this route" once per generation; native
  Vulkan says it once per process. The bridge would be the cheapest next route (same code, same UI-alpha decision)
  but is not promoted without its own decision. The driver proxy says unavailable: it returns before the resolve.
- **NR disabled.** The entry points that find the pass switched off already leave a flag for the next Dispatch
  (`g_resetOnReturn`); the inspector reads it, behind its gate, where Dispatch takes it. The NR-disabled paths are
  not touched.
- **Malformed values** are kept as written on save (lowercased, as every string key is), not rewritten to `off`.

#### What a game run has to show, and has not

None of this has run in a process with a GPU. Still to be shown on the D3D12 present route (`HookMethod=2`), on
Proton and on native Windows separately: that the unit compiles and links with MSVC; that with the key at `auto`
the log has no `mask inspection` line and frame times and the picture are what they were; that with `log` the first
line appears on the first recorded frame and matches the settings in the menu; that toggling `UiProtection`,
`AutoMask`, `SkinStructure`, `HighlightRecovery`, `ResidualLimit`, `MinRatio`, `ApplyModel`, Compare and the debug
views each produce one line within about two seconds and no others; that a resize, a preset change and the NR
toggle key each start a generation; that an after-upscale or bridge title says unavailable once and a Vulkan title
once per run; that a session with frame generation or model cadence on does not alternate lines; and that the line
count stays far below the budget in an ordinary session. The UI alpha being handed says nothing about whether the
mask covers the HUD: that needs the mask view of the follow-up below.

### Optional observational follow-up, still before regional edits

Only after slice 0 establishes where data exists, design an extension of the existing debug-view surface to show
UI alpha and the composition's nonsemantic weights. Reuse existing view selection; do not create parallel
controls for the same quantity. This may need a bounded diagnostic target because scalar weights are not exposed
textures. Any new constants remain append-only and synchronized; regenerate both DXIL/SPIR-V when the common
shader changes. Unsupported modes do not become selectable. English and pt-BR labels name the actual source.

Its diagnostic output must be downstream of model input and temporal history, including capture routes that
could otherwise see the overlay on the next frame. Pairing, lifetime and output-format contracts must be reviewed
before code. No view of the model's internal skin mask until an actual export is established. Observational overlay
pixels are deliberate display changes, but never permission to change the NR processing mask or regional strength.

### Gate before a second slice changes regional NR

The following are proposed acceptance thresholds, **not measured results or owner-approved budgets**. A later
regional design must name its insertion point relative to UI restoration, highlight recovery and final darkening
floor, and preserve all of them. It must not simply multiply a global model parameter by a bounding box.

| Measurement | Required evidence before promoting regional edits |
|---|---|
| Value beyond existing settings | Paired close-up and whole-character clips where AutoMask/SkinStructure, ordinary strength and existing protections cannot satisfy the stated request. Owner judges a blinded comparison against that tuned baseline, not only NR defaults |
| Timing and pairing | Every accepted mask matches the real frame, route, extent/crop and device generation of the original/NR pair. Any asynchronous provider must prove transport/disocclusion or refuse. Zero accepted wrong-generation, future, cut-crossing, FG-only or uncompleted samples in fault fixtures; report refusal coverage too |
| Spatial accuracy | Annotated character silhouettes, not boxes: proposed precision at least 99% and recall at least 90% on each reviewed clip; disclose boundary errors separately. Include skin tones, clothes/hair, shadows, tiny/stylized subjects, multiple crossing people, occlusion, camera cuts, HUD and static scenes. No pooled average hiding a failed clip |
| Visible benefit and stability | Repeated local A/A to establish noise, then A/B at equal model/route/settings. Report target and background deltas, silhouette-boundary error and motion-compensated temporal change. Reject visible halo, identity switch, HUD damage or worsened flicker even if aggregate metrics improve |
| Cost on both target machines | Windows/RTX 3060 and Proton/RTX 4090 separately, fixed reproducible scene and present route: proposed incremental p95 CPU at most 0.10 ms/frame and GPU at most 0.20 ms/frame, no new synchronous Present wait, p95 whole-frame time increase at most 2%, bounded memory. Report p99 and worst cut/resize spikes. Slice 0 itself adds zero GPU allocations/commands; a later overlay/mask proposes an explicit memory cap before implementation |
| Off/refusal equivalence | Same-binary off versus baseline output byte-identical where the harness provides deterministic input; otherwise first bound A/A noise and retain exact command/parameter traces. Missing/invalid mask follows existing NR, not a forced full-frame original replacement. NR disabled, strength zero, passthrough, signed HDR, multipass and resize remain covered |

Use the [route validation matrix](../../maintenance.md#validation-route-matrix). Start with one named route and
keep all others unavailable. Native Windows D3D12 validation and Proton GPU runs are separate gates; native Vulkan,
bridge and window-capture paths require their own evidence before exposure. Pin source, dirty state, binary/model
hashes, API, resolution, working scale, frame-generation setting and test sequence. None of this evidence exists
for the proposed feature today. A failing gate keeps regional editing deferred; do not relax it to claim parity.

## 5. Host validation plan and current verification boundary

Slice 0 has a small pure observation policy with a fake clock and logger
([nr-mask-inspection](../../../tests/nr-mask-inspection/README.md)). It exercises the config round-trip; off and
NR-off zero side effects, with allocations counted; repeated unchanged status; two-second coalescing; time rollback;
unsupported route; UI mask disabled/not produced; internal skin mask unavailable; failed recording; turn-off with
pending status; and resize/rebuild/pass/route generations. Its seeded mutants independently remove each admission
and off gate. Source wiring checks accompany, not replace, behavior tests: the statements added to `Dispatch` are
also compiled and run as written. No texture coverage is asserted on the host.

For later masks, synthetic silhouettes/depth/motion/crops exercise transforms and refusal. A fake detector can
test late replies without downloading a model. Tests of a shader's math cannot certify its bytecode bindings,
barriers, GPU lifetime, skin recognition or visual usefulness. Preserve the GPU completion contracts even in a
debug-only path. The design-only acceptance commands are:

```bash
python3 tools/check_docs.py
python3 tests/repo-privacy/run.py
systemd-run --user --scope -q -p MemoryMax=6G -p MemorySwapMax=1G -p OOMPolicy=kill -- python3 tests/run_all.py --only repo-docs,repo-privacy --jobs 1 --strict --log-dir tests/repo-docs/artifacts/char-inspector
```

These check documentation/registry/privacy, not implementation or the technical truth of the analysis. No Wine,
MSVC, dxc, game deployment, model download or detector execution belongs to this task.

## 6. Owner decisions still open

1. First benefit: **understand current protection / gentler skin / whole-character strength / visible object boxes**?
2. Accept **log-only slice 0, then mask visualization** as the first scope: **yes/no**? Slice 0 was built on
   2026-11-20 on the author's recommendation, with the owner's answer still to come at review; the visualization is
   not started.
3. For future regional edits, target **all valid characters / one persistent named character**? The second needs
   identity evidence beyond a transient track ID.
4. May an optional future local detector receive frame pixels outside the game process, with no network or saved
   images: **yes/no**? “No” removes A; it does not remove B/C.
5. Which route qualifies first: **D3D12 present / D3D12 after-upscale / native Vulkan**? Recommendation: D3D12
   present, where existing UI-alpha inspection has the clearest immediate use; no automatic expansion to all routes.
   Slice 0 took the recommendation: every other route reports unavailable.
6. Accept the proposed cost/precision budgets as the initial go/no-go bar: **yes/no**? If not, choose **stricter cost /
   greater coverage at higher cost** before a regional implementation plan.

### Claims left unconfirmed

No independent confirmation here of NeuRotic's distributed installer inventory, real-game detection accuracy,
actual frame-time/RAM/VRAM costs, Linux/Proton worker operation, Windows GPU correctness, or release timing for
regional effects. No per-object NR renderer consumer was found; no exposed skin-mask texture was found in
Retoque. These absences are bounded to the named snapshots, not predictions about later versions. Existing
experimental code, stored effect fields and upstream segmentation outputs must not be described as delivered
regional rendering. Slice 0 is implemented and host-tested only; every later step and all runtime measurements
recommended here remain pending review.
