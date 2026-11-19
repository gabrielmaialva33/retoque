# The game's own depth for the present pass, in titles that call no upscaler

Status: **revalidated, implementation blocked on the diagnostic contract**, 2026-11-19, against
`6972e2d7`. The owner reopened step 0 on this date. No observer, config key, depth copy or game run is delivered
by this revalidation. The original design was written on 2026-10-14 against `27f89456`.

Retoque source anchors below refer to `6972e2d7`. The NeuRotic comparison is explicitly historical, against
`7132a103`; it is not a claim about that project's current implementation. Ideas only were studied; no source,
tables or strings from that project are adopted.

Read first: [present-real-guides.md](present-real-guides.md) (the guide ring, its identity rules and the boundary count
this note reuses), [nr-present-hook.md](nr-present-hook.md) (the present host),
[synthesized-motion.md](synthesized-motion.md) (what feeds the motion guide today),
[nr-before-dlssg.md](nr-before-dlssg.md) (the boundary at the game's own `Present`, and the pattern of a measured,
stepped note), [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md) (the D3D11 host),
[reduced-scale-guides.md](reduced-scale-guides.md) (guide formats and sizes, and the native-Windows lesson) and
[DEVELOPMENT.md](DEVELOPMENT.md).

## Revalidated 2026-11-19

The heuristic in option (a) remains an experiment worth measuring. The current tree does **not** establish its
precision or make observed depth safe to use. This audit did not find a later implementation of the scanner.
It did find unresolved specification choices that prevent implementing step 0 *exactly as written*. These are
separate from the source moves below: the missing no-upscaler clock also existed at the original baseline.
They must not be described as a new renderer regression or as proof that heuristic discovery is impossible.

### What moved and what no longer holds

| Original assumption / anchor | Current source and correction |
|---|---|
| All present-guide code in `DlssNr_Dx12.cpp` | Cloning, capture, stamping and identity moved to `retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp`: `CreatePresentGuideClone` at 92, `StampPresentGuide` at 170, `AskGuideIdentity` at 230, commit at 823 and `CaptureTemporal` at 842. `ReadableGuide` moved to `DlssNr_Dx12_Resources.cpp:405`; `GatherGuideFrame`/`GatherFrame` to `DlssNr_Dx12_Inputs.cpp:102`/156. |
| The bridge's zero-guide paragraph | It describes only the fallback. `retoque/dlssnr/DlssNr_PresentHost.cpp:704` asks `CapturedPresentGuides`; accepted pairs replace both zeros at 722. The native D3D11 guide transport exists too; it is not heuristic DSV discovery. |
| All motion estimation is FidelityFX | The default software estimator still exists, but D3D12 also selects NVIDIA Optical Flow and can share synthesized-FG estimates (`DlssNr_Dx12.cpp:5961`, 6028). Both leave fallback depth zero. |
| No tag hooks outside FG input | No longer true. `OptiScaler/hooks/Streamline_Hooks.cpp:2634` arms the tag inventory with NR enabled; 2655-2665 attaches note-only tag hooks without a Streamline FG input. Constants outside that input are still not observed by this branch. Step 0c is partly superseded, not fully implemented. |
| Every stale capture forces a skip | No longer true at the game's own boundary with optional guides. `DlssNr_Dx12.cpp:5127` keeps the old skip only below the boundary or with `RequireDlss`; the boundary can continue on zero/synthesized guides. The existing ring still must not be overloaded with depth-only captures. |
| Status latches on the first capture | Routing still uses `g_temporalValid`; status does not. `DlssNr_Dx12.cpp:4412` uses recent guide status and gives the own-Present boundary precedence. There is no universal status string that proves zero upscaler captures. |
| Step 0 creates no D3D12 objects | It must record no GPU command and create no GPU work/resources for observation. Obtaining optional command-list interface vtables may need temporary allocator/list objects; `D3D12_Hooks.cpp:1065` already uses that installation technique. Reuse that setup where possible and document any extra installation objects. Native Windows must check hook correctness, forwarding and coexistence, not merely absence of a crash. |
| Step 0b with real motion tests synthesized motion | It isolates depth with **real** motion. It cannot settle the model's acceptance of real depth plus synthesized motion, nor the all-zero-guide case. Separate factorial arms are needed for that question. |

### Decisions required before implementing step 0

1. **A frame clock that exists without an upscaler.** `AskGuideIdentity` returns at
   `DlssNr_Dx12_PresentGuides.cpp:236` before `Tracker::AtPresent` whenever the ring has never recorded.
   `Tracker::AtPresent` (`DlssNr_PresentGuideIdentity.h:414`) is the only increment of its epoch. Thus the
   exact target arm in this design has no advancing identity boundary. The present route also returns before
   its pick on warm-up, stand-down or allocation/admission failure. Merely reading `Tracker::Epoch()` would
   combine multiple frames into one interval. Proposed: a **log-only boundary clock**, advanced at the owning
   real-present entry after duplicate/FG stand-down and before optional NR work, carrying the existing identity
   epoch as separate metadata when available. Changing the identity tracker's existing clock instead needs
   explicit tests that its captured-guide lag/consumption behavior remains unchanged. This is a diagnostic
   contract choice, not permission to alter the pass's pairing rules.
2. **Unknown touched set.** An `ExecuteCommandLists` containing a list recorded before installation provides
   the list and queue, not its resource references. The existing hook (`ResTrack_dx12.cpp:693`) does not retain
   commands. The rule “only the resources it touched” cannot apply when that set is unknown. Proposed: preserve
   the observed candidates, but reject the interval as `unobserved` on an unknown list on its presenting queue;
   apply resource-local refusals only where the touched set is actually known. A skipped/unknown recording must
   never improve the apparent precision by silently disappearing. Internal lists need positive identity, not a
   thread flag at submission time, and bounded record/descriptor overflow needs the same refusal.
3. **R1 and R2 are different decisions.** “Would pick” currently means R1 after all rules, including R2's tie
   margin. The requested ground-truth field contains only one score, yet the decision to keep draw hooks asks
   to compare **R1 precision with R2 precision**. Proposed: compute each ranking/refusal independently, report
   `r1 matches / r1 eligible comparisons`, `r2 matches / r2 eligible comparisons`, and their agreement. Retain
   the total ring-commit count separately. `matches / captures` is not precision: multiple captures, refused
   identities and frames with no eligible comparison must not enter that denominator. Original resource
   identity must accompany each committed slot; neither a clone pointer nor the latest unsubmitted evaluate
   is ground truth. DSV view/subresource identity must also agree before an equal resource pointer is counted.
4. **The diagnostic must refuse missing lifetime identity.** The old fallback to raw pointer identity on a
   missing destruction notifier can count an address reused by a different resource as a match. Proposed:
   `unobserved` unless an object generation is known; do not silently claim the same confidence as a tracked
   object. This remains log only and retains no game resource. Resource and descriptor-heap reuse, Reset/Close
   failures, replayed lists and partial hook installation need explicit contracts before reporting scores.

These choices affect the first slice's required counters, interval, refusal results and new hooked calls. They
are raised to the lead rather than resolved by silently weakening the requested “exactly” contract. No new hook
is installed by this documentation change. Step 1 remains blocked on both valid step-0 measurements and step 0b.

The implementation proposal is: first approve these four amendments; then add the pure ranking/refusal/window
header and adversarial event tests; then add a bounded D3D12 adapter with transactional installation, no lock
held across a game call, and per-recording hot-path storage. The off path must not install any observer detour or
allocate observer state. This proposal is **not** an implemented or host-tested capability.

## The problem

A title that calls no upscaler never hands OptiScaler a depth buffer or motion vectors. The present pass still runs
there (`HookMethod=2` on D3D12, or an admitted D3D11 bridge host), but on guides it made up:

- **Depth is a constant.** `RunPresentPass` (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp:4788`) takes the zero-guide branch
  when the ring offers nothing: `s_dummyGuides.RecordClear` (`:5276`) and `PresentFrameDefaults`, which says
  `DepthInverted = false` (`:6260`). The model sees the whole frame at depth zero, uninverted, every frame.
- **Motion is zero, or synthesized.** With `[DlssNr] SynthMotion` the selected estimator
  (`retoque/shaders/synth_motion/`) estimates it from `g_bbCopy` (`DlssNr_Dx12.cpp:5354`). Without it, motion is zero and
  `ZeroGuideReset` (`:5290`) is the opt-in answer to the ghosting that causes.
- **Supersampling is clamped to native** on that branch (`frame.AllowSupersampling = false`, `DlssNr_Dx12.cpp:5286`), because the
  model refuses zero guides above 1.0. Whether real depth plus synthesized motion would lift that refusal is
  synthesized-motion.md's §9 question 1, still open.
- The D3D11 bridge's host does the same with its own zeros (`retoque/dlssnr/DlssNr_PresentHost.cpp:692-758`).

The question is whether the game's real depth can be found without an upscaler's parameter block, and whether motion
can come with it cheaply. Two things are not known and decide everything below:

1. **How much depth is worth to this model.** No measurement in this tree separates depth's contribution from motion's.
   The only comparison of real against made-up guides (nr-dx11-bridge-host.md, 2026-09-18; synthesized-motion.md,
   PCSX2) changed motion, not depth.
2. **How often a heuristic would pick the wrong buffer.** A wrong depth with the right extent is not neutral: it
   guides the model with edges that are not in the picture.

## What the tree observes today

| Hook | Installed when | Sees depth? |
|---|---|---|
| `CreateCommittedResource`, `CreatePlacedResource` (`OptiScaler/hooks/D3D12_Hooks.cpp:1855`, 1896; slots 27/29 at 2421/2422) | NR enabled or Unreal atomics spoof at device hook time (2474-2499) | Resource descriptions/pointers and placed heap reach `ExposureScan::NoteResource` (1888, 1924). No observer receives initial state or placement offset. Newer creation variants and reserved resources are not captured here. The spoof's early-return branches bypass these notes. |
| `CreateDepthStencilView` | Never attached by ResTrack: slot 21 is commented out (`ResTrack_dx12.cpp:2738`) | No DSV-to-resource map. |
| `OMSetRenderTargets` (`ResTrack_dx12.cpp:1833`) | HUD fix under `FGInput=upscaler`, no interop (`D3D12_Hooks.cpp:2524`; attach at `ResTrack_dx12.cpp:2535`) | Depth argument is forwarded at 1866, 1878 and 1978, not interpreted. |
| Draws, `ClearDepthStencilView`, `ResourceBarrier`, `BeginRenderPass`, `ExecuteIndirect` | Direct draw hooks with HUD fix (`ResTrack_dx12.cpp:2544-2547`); no depth observer for the others | No scene-depth event stream. Calling a barrier helper is not hooking the game's barriers. |
| `ExecuteCommandLists` (`ResTrack_dx12.cpp:693`) | NR submission tracking (`EnableNrSubmissionTracking`, 2628), or HUD tracking | Lists/queues and submission evidence, no recorded depth contents. |
| `slSetTag`, `slSetTagForFrame`, `slSetConstants` | FG input `dlssg`/`nvngx`; note-only tag hooks additionally with NR enabled (`Streamline_Hooks.cpp:2634-2665`) | FG input reads depth tags (`inputs/FG/Streamline_Inputs.cpp:326`). The NR inventory records declarations and shape, not usable resource/state/queue rights; no note-only constants hook in this branch. |
| DLSS-G NGX evaluate (`inputs/NVNGX_DLSS_Dx12.cpp:1254`) | When a game's intercepted feature is FrameGeneration | The probe is fed at 1270. The nvngx FG route reads `DLSSG.Depth`, motion and inversion (`framegen/nvngx/Nvngx_FFX.cpp:408-414`); this is not a present-depth capture. |
| D3D11 device/context (`hooks/D3D11_Hooks.cpp`) | Device sampler and Release hooks, plus existing bridge/guide integrations | No heuristic DSV/bind/clear observer. Native upscaler guide transport is a distinct, already implemented path. |

**A correction to nr-without-game-dlss.md.** Its table says `ResTrack_dx12.cpp` "already *sees* every depth-stencil
descriptor the game binds", so only the heuristic is missing. Read from the code, that is not so. The hook is attached
only for the HUD fix under `FGInput=upscaler`, it never reads the depth argument, and with no `CreateDepthStencilView`
hook the descriptor would name nothing anyway. Discovery needs new hooks: DSV creation, descriptor copies, binds,
clears, barriers and probably draws.

## Historical prior art: NeuRotic at `7132a103`

Below, **claim** marks what a comment or document says and **read** marks what the code does. Neither is evidence
that the result looks right in a game.

### The line-1821 observer is a diagnostic, not the discovery

*Read:* `hkCreateCommittedResource` and `hkCreatePlacedResource` (`OptiScaler/hooks/D3D12_Hooks.cpp:1813-1890`) open a
`CandidateObserver::CreationScope` and emit every creation, with its descriptor, heap and initial state, into a bounded
JSONL sink. The reducer ranks entries by "depth supported" first: the `ALLOW_DEPTH_STENCIL` flag, or format D32S8, D32,
D24S8 or D16 (`nr/diagnostics/candidate/Contract.h:94-96`). Then come fewer "cautions" (array or mips above 1), then the
newest (`Reducer.h:93-98`). It has no size, sample-count or use test.

*Claim:* "OBS-CLOSURE-1: descriptive CPU values, never resource rights" (`Contract.h:2`). *Claim:* "the product has no
TryRead consumer" (`Observer.cpp:15-16`). *Read:* nothing outside the diagnostics reads the snapshot, so it chooses no
depth.

### The discovery: `OptiScaler/dlssnr/NativeD3D12Guides.cpp`

*Read*, all in that file unless named:

- **Hooks** (`:182-206`). Device `CreateDepthStencilView`, `CopyDescriptors` and `CopyDescriptorsSimple`. List
  `OMSetRenderTargets`, `DrawInstanced`, `DrawIndexedInstanced`, `ClearDepthStencilView`, `ResourceBarrier`, `Reset`,
  `Close` and `ExecuteBundle`, plus List4 `BeginRenderPass` and List7 `Barrier`. Queue `ExecuteCommandLists`. The
  vtable slots are read from a scratch list and queue (`:188-191`). Attaching suspends every other thread in the process
  through a Toolhelp snapshot (`:193-200`).
- **Installed by default.** `Install` is called from the device hook with no `Enabled` check
  (`hooks/D3D12_Hooks.cpp:2373`). Observation is on whenever the input source is Automatic or Built-in, and Automatic is
  the default (`dlssnr/NativeGuideRoute.h:17-26`, `:62-67`). The hooks run whether or not NR is on.
- **What can be a candidate** (`:84`, `:110-116`): a Texture2D with one mip, one array slice and one sample, with
  `ALLOW_DEPTH_STENCIL`, given a DSV of mip 0. At most 64 resources and 1 GiB, held by strong reference (`:32-35`,
  `:84-88`).
- **Per command list** (`:133-147`): a bind sets the list's current depth. A draw while it is bound adds
  *vertices x instances* (not draws, and not filtered by depth write, which D3D12 keeps in the PSO). A full clear (no
  rects) to 0 records "reversed", to 1 "forward", anything else "unknown". A transition barrier on a depth-stencil
  resource records its state, and is "known" for flag-free barriers on subresource 0 **or** the whole resource. An aliasing barrier, a bundle,
  an enhanced barrier, a render pass or more than 256 events marks the whole list unsupported (`:145`, `:150-153`).
- **At submission** (`:155-168`): only DIRECT queues on the observed device. Events apply in order to a per-resource
  candidate. A clear zeroes the count. A barrier's state is trusted only if its "before" matches the last known state,
  and the first barrier seen makes it known: **the creation state is never used**, so a depth buffer that is never
  transitioned is never known. One unsupported or unrecorded list in a submission **clears every candidate** (`:159`).
  The hook holds the observer's mutex across the real `ExecuteCommandLists` (`:156`).
- **At present** (`:221-277`, called from `wrapped/wrapped_swapchain.cpp:255`). SDR only (`:235-236`). A candidate must
  have been submitted on the presenting queue, with a known state and a non-zero count, at the backbuffer's exact
  extent. The highest count wins, and only if it is at least 3 and the runner-up is at most half of it (`:240-244`).
  Direction comes from the clear or from the override key; unknown refuses: "Depth direction requires endpoint clear or
  explicit override" (`:245-246`). A change of resource, size or input epoch starts a new generation, that is, a history
  reset (`:258-260`). Counts are consumed at every present.

**How it decides it is the scene depth.** Extent equal to the backbuffer (shadow maps, cascades and mip chains fall
out), then the most geometry since the last clear inside the present interval, with a 2:1 margin. Nothing names UI or
a first-person pass. A full-size 3D preview drawn last with more geometry would win. The D3D11 version is stricter about
what counts: only draws with depth enabled and the write mask `ALL` (`NativeD3D11Guides.cpp:158-170`), and a tie
refuses (`:112-119`).

**The copy** (`dlssnr/connections/D3D12Connection.cpp`). It signals a fence on the presenting queue and **waits on the
CPU for it**, up to 2 s (`:239-240`, `Completed` at `:19-24`). Then, on a private DIRECT queue, it moves the game's
depth from its tracked state to `COPY_SOURCE`, `CopyResource`s it into a typeless twin, and restores the state
(`:248-249`). A compute pass normalizes it to `R32_FLOAT` (`:127-133`), followed by a second CPU wait (`End`). D32S8 is
not among the formats it maps (`:29-37`), so that format is refused.

**Reversed-Z and linearization.** Not linearized: device depth plus a reversed flag (`NativeD3D12Guides.cpp:261`), the
DLSS convention. The CPU path checks depth is finite and inside [0,1] (`NativeGuideProcessor.cpp:53`). The user can
override the direction (`NativeDepthDirection`, `NativeGuideRoute.h:48-50`).

**Motion.** Derived, never observed: FidelityFX optical flow, the software backend (`NativeGuideProcessor.cpp:144`). It
runs at full resolution into `R16G16_FLOAT` current-to-previous pixels, plus an `R8` "distrust" mask
(`addons/prepared-guides/flow/PreparedFlowD3D12.h:39-41`), on a private queue that waits the producer's fence
(`:66-67`). Then it **waits on the CPU** (`NativeGuideProcessor.cpp:307-310`; `Wait` defaults to 5 s). Input must
be 256x64 to 4096x2160 (`flow/FlowMath.h:23-26`), so 5120x1440 is refused. An NVIDIA Optical Flow backend exists but
this route does not ask for it.

All of it runs at present, under the same mutex the draw and submit hooks take, with up to three CPU waits on the
present thread. While the pass runs, every game thread that draws or submits waits behind it.

### Require Guides and Auto Guides

*Read:* `PresentInput::Policy` is `ImageOnly`, `RequireGuides` or `AutoGuides` (`dlssnr/DlssNr_PresentInputPolicy.h:9`).
It governs the Present route's use of the game's own upscaler guides ("Native"), matched to the present by
`PresentGuides::MatchMetadata` (`DlssNr_Present.cpp:1226-1240`). `PresentInputDecision::Choose`
(`DlssNr_PresentInputDecision.h:22-43`) decides:

- `RequireGuides` with no match refuses the frame, and it is shown as the game drew it ("Required Native guides
  unavailable").
- `AutoGuides` with no match runs image-only: constant depth and zero motion (`DlssNr_Present.cpp:1551-1554`). The
  workload falls back to full output when the render size is unknown.
- A change of input class resets history (`ChangesHistory`).

The built-in capture is a separate input source, not a policy value. It claims the connection before it processes a
frame (`NativeD3D12Guides.cpp:247`), and while a prepared owner holds it, `OwnsPresentOutput()` is true
(`PreparedGuideRoute.h:22-25`) and the Present route reports "Prepared guides (addon owned)" instead of running
(`DlssNr_Present.cpp:852`, `:868-901`). When the capture refuses, it publishes a reason and the Present route's policy
applies as above. Some refusals latch until a restart: another swapchain, a queue or device change, and any failure
it quarantines (`NativeD3D12Guides.cpp:226-227`, `:230`, `:253`).

### Does it work, by its own record

- *Claim:* "Added capture routes for available depth and motion guides in supported games without native DLSS inputs.
  Guide availability depends on the capture path." (`RELEASE_NOTES.md:47`)
- The no-DLSS capture title it names is **Detroit: Become Human, on Vulkan** (`:85`), which is a different file
  (`NativeVulkanGuides.cpp`, render-pass scopes with explicit layouts).
- The "reported working" list (`:50-80`) does not say which route each title ran on. No D3D12 title is named as
  running on built-in capture.
- Its own success message reads "Captured submitted scene depth and derived motion; exact output copied back
  (estimated scene association)" (`NativeD3D12Guides.cpp:273`).
- The test-only accessors (`NR_NATIVE12_TESTING`, `:278-283`) are called by no test in the clone.

So the D3D12 discovery is unproven by NeuRotic's own account.

### What to take, and what not

Take the ideas: DSV-based candidacy, the backbuffer-extent filter, weight counted since the last clear within one
present interval, a margin over the runner-up, direction from a full clear to 0 or 1, refusing an unknown direction
and an unknown state, and the D3D11 depth-write filter. Do not take:

- the default-on install, which is not inert when off;
- one mutex across the game's submissions and the pass's CPU waits;
- strong references to game resources;
- wiping every candidate because one list was unobservable;
- ignoring the creation state.

## Design options

### (a) Heuristic discovery: DSV creation, binds and clears, the last full-size depth before present

**What is observed** (new hooks, installed only under the gate in "Config"):

- Device `CreateDepthStencilView` and `CopyDescriptors`/`CopyDescriptorsSimple` for DSV heaps: a map from DSV handle to
  resource, with the view's format, mip, slice and read-only flags. This is the only way to name what a bind names.
- The creation hooks that exist (`D3D12_Hooks.cpp:1855`, `:1896`) receive the initial state, and heap plus offset for
  placed resources. Their current exposure notes do not retain state or offset; the proposed adapter must do so.
- List `OMSetRenderTargets` and `BeginRenderPass` (writable binds; a render pass's `CLEAR` beginning access is a clear),
  `ClearDepthStencilView`, `ResourceBarrier` (transitions and aliasing), List7 `Barrier` (noted, not followed), `Reset`
  and `Close`.
- Optionally the draws (`DrawInstanced`, `DrawIndexedInstanced`, `ExecuteIndirect`, `DispatchMesh`, `ExecuteBundle`),
  as a weight.
- Queue `ExecuteCommandLists`, through the existing hook's slot, for execution order per queue.

**Per present interval**, defined by the identity layer's boundary count (present-real-guides.md, step 0 as built,
item 1), so that under the game's DLSS-G it is the interval between the game's own presents, not DLSS-G's:

- **Admit** a resource that is a Texture2D, single-sampled, with a DSV of mip 0 on a non-array, at the backbuffer's
  extent, bound writable in this interval, on lists executed on the presenting queue.
- **Rank** two ways, and log both in step 0:
  - R1, **the last full-size depth bound writable before present**, the rule this option is named for.
  - R2, **the most draws while bound writable since its last clear**, inspired by NeuRotic's geometry-weighted ranking; counting draw calls here is a separate policy, not its vertices-times-instances score.
- **Direction**: a full clear to 0 means inverted, to 1 not inverted; anything else is unknown.
- **State**: the creation state, then every barrier in execution order on the presenting queue. Unknown if a barrier
  ran on another queue, if an enhanced barrier touched it, or if it was never seen.

R1 is cheap (no draw hooks) but fooled by whatever full-size depth is bound last: a first-person pass with its own
buffer, an inventory's 3D preview, or a depth-copy pass (harmless, since that is scene depth). R2 needs per-draw hooks
and is blind to GPU-driven engines whose geometry is `ExecuteIndirect`. Step 0 measures how often they agree, and
against what (see "Steps").

**The copy, in step 1: at present, on the present list.** The pass records:

1. a transition of the pick from its tracked state to `COPY_SOURCE`;
2. `CopyTextureRegion` of subresource 0 (plane 0) into an owned clone of the same format, made the way
   `CreatePresentGuideClone` makes them (`DlssNr_Dx12_PresentGuides.cpp:92`);
3. the transition back.

The pass then reads the clone through `ReadableGuide` (`DlssNr_Dx12_Resources.cpp:405`) as it reads a typeless upscaler guide, with
`DepthInverted` from the direction, the full extent and no subrect. The present list runs on the presenting queue after
everything the game submitted there before `Present`, so the copy is ordered by the queue alone. It needs no wait, and
the capture belongs to this frame by construction.

**Refuse** (the frame keeps constant depth) on any of:

- no candidate;
- extent;
- multisampled;
- a tie (R2's runner-up above half);
- direction unknown;
- state unknown;
- cleared after its last use in the interval (at present it holds a clear, not the scene);
- aliased: an aliasing barrier naming it or its heap after its last use;
- written or transitioned on another queue;
- an unobserved construct on the pick (a bundle drawing into it, an enhanced barrier, a list recorded before the
  hooks).

For an event whose touched resources are known, refusal stays resource-local. The original promise that **any**
unobservable list costs only its touched resources is not implementable for a list whose contents were never
observed. The proposed conservative interval refusal is a pending decision in the revalidation section.

**Proposed cost contract, not a measurement.** Hooks on every bind, clear and barrier, and on every draw if R2 is kept: one lookup in a per-list record owned
by the recording thread, no lock, no allocation. Execute merges the records under a short leaf lock that is never held
across the game's call or any wait. At present there is one copy of about 20 MB at 3440x1440 for `R32` depth (40 MB for
D32S8). That is half of the two copies nr-present-hook.md already costs per processed present.

### (b) Only where a depth tag already exists

Some titles that run no upscaler still hand someone their depth.

- **(b1) Streamline tags.** A game running DLSS-G with super resolution off still has to tag `kBufferTypeDepth` and
  `kBufferTypeMotionVectors`, because DLSS-G cannot run without them, and set `slSetConstants` (depth inversion,
  motion scale). A tag says what the resource is, its lifecycle, and the state it is in (`sl::Resource::state`).
  - The capture is `CaptureTemporal`'s pattern: copy on the tag call's command list, stamp, commit. For `eOnlyValidNow`
    that is required; for `eValidUntilPresent` it can wait for present.
  - This is the only route that brings **real motion** with the depth.
  - The note-only tag hooks already attach outside the FG-input route (`Streamline_Hooks.cpp:2634-2665`).
    Using tags would still require a separate capture/lifecycle contract and constants observation. The inventory
    is not permission to dereference a tag later at present (also relevant to nr-before-dlssg.md's H2/H3).
- **(b2) DLSS-G's NGX evaluate parameters.** `DLSSG.Depth`, `DLSSG.MVecs` and `DLSSG.DepthInverted` reach our evaluate
  hook on every DLSS-G frame. **Rejected:** nr-before-dlssg.md step 0 and 0b measured that DLSS-G evaluates from
  another thread, on a queue of its own, after the game's `Present` has returned. Frame N's guides arrive after
  present N has been edited, so they could only guide frame N+1, which is the pairing rule 6 of present-real-guides.md
  exists to refuse.
- **(b3) FSR-FG or XeFG inputs** of a game that runs frame generation without upscaling (`inputs/FG/FfxApi_Dx12_FG.cpp`,
  `inputs/FG/XeFG_Inputs_Dx12.cpp`). This is the same shape as (b1). Those routes are wired only when OptiScaler takes the game's
  FG as its input.

(b1) is robust where it applies, and it applies to few titles: those with an FG integration whose user turned the
upscaler off. Crimson Desert may be one, if its menu allows DLSS-G without an upscaler (open question 4).

### (c) Nothing beyond synthesized motion

Depth stays constant and motion comes from the estimator. There is no new hook, no wrong-buffer risk and no cost. It is
the right answer if depth turns out to matter little to the model, which nobody has measured (step 0b).

### Compared

| | (a) heuristic | (b1) tags | (c) nothing |
|---|---|---|---|
| Titles | any D3D12 title (D3D11 separately) | FG integrations with upscaling off | all |
| Says it is depth | no: inferred | yes: declared | n/a |
| State at copy | tracked, can be unknown | declared by the tag | n/a |
| Direction | inferred from a clear | declared in constants | n/a |
| Motion | still synthesized | real, from the same tags | synthesized |
| Wrong-buffer damage | possible; refusal rules bound it | only if the game mis-tags | none |
| New hot-path hooks | binds, clears, barriers, maybe draws | tag calls, a few a frame | none |
| Shared with other work | none | nr-before-dlssg H2/H3 tag observation | n/a |

### Recommendation

**(c) remains the behaviour. Build (a)'s step 0 now, log only, and score it against the answer in titles where the
answer is known. Take (b1) first wherever a title tags depth.**

- **Why not (a) straight away.** Its whole risk is a wrong pick, and there is no way to judge a heuristic without data.
  The data can be had cheaply. A title running DLAA has an upscaler, and the upscaler's own depth parameter *is* the
  scene depth at the backbuffer's extent. Step 0 compares its pick with it on every frame: precision, measured. It
  also compares the inferred direction with the game's `DepthInverted` create flag.
- **Why (b1) first where it exists.** It declares everything (a) has to guess, brings real motion, and fits the ring
  and its identity rules unchanged.
- **Why not stop at (c) yet.** The value experiment (step 0b) is cheap and answers synthesized-motion.md's open
  question 1 on the way. It runs beside step 0, and step 1 waits for both. If it shows depth is worth nothing
  visible, this note ends at (c), and says so.

## Fit with the guide ring and the identity rules

**Copy at present (step 1 of (a)): not the ring.** The ring is the upscaler's, and three things say keep the observed
depth out of it:

- **The skip rule.** Below the game's own boundary, and at the boundary with `RequireDlss`, no newer capture
  still skips (`DlssNr_Dx12.cpp:5127`). At the own boundary with optional guides this no longer happens. A
  depth-only capture must not arm the remaining upscaler-specific skip paths or change the capture/identity
  history; the empty-ring baseline is still important.
- **Slots hold pairs.** A ring slot is a depth and motion pair (`PresentGuideClone`), and the motion here is made at
  present from the same frame.
- **No identity question.** A copy recorded on the present list at the present has lag 0 and is the present's own.
  Rules 1 (device), 3 (swapchain), 5 (consumed), 6 (lag) and 7 (age) hold by construction. Rule 2 becomes "the pick's
  extent equals the backbuffer's", which is an admission test above. Rule 4 becomes R2's tie margin. Rule 8 has no
  subrect to check.

The observed depth takes an owned clone per present-list slot, retired by submission evidence like `g_bbCopy`. The
stamp is the identity Tracker's boundary count (`DlssNr_PresentGuideIdentity.h:414`; subject to the no-upscaler clock decision above), so the identity log can count
"observed depth" as a third source beside the two consumers.

**Copy at the bind point (step 3, only if step 0 shows the depth cleared or aliased before present).** That is "a
capture made at a bind point with a boundary count":

- The copy is recorded in the game's own list, at the transition that ends the scene depth's writes (or before the
  clear that destroys it, NeuRotic's D3D11 move), for the resource last frame's pick named.
- It goes into a **second `PresentGuides::Ring` instance** (the class is generic), with depth-only clones.
- The boundary count is taken at **`ExecuteCommandLists`** of that list, not at recording, because a multithreaded
  engine records a frame ahead. That differs from `StampPresentGuide`, whose evaluate list is recorded and submitted in
  the same interval.
- Rules 1 to 7 then apply as written, with the depth's extent standing for the output's in rule 2, and rule 6's lag
  expected at 1 (measured so at the boundary in Crimson Desert). The skip rule must stay keyed on the upscaler's ring
  alone.
- Copies are illegal inside a render pass and in a bundle, so such a pick is refused, not copied.

**(b1) tags: proposed reuse of the existing ring.** Once a tag capture meets its lifecycle and identity rules, it would be shaped like an upscaler capture:
depth and motion, copied on the game's list, stamped, committed, and picked by `PickForPresent` and the eight rules.
`g_temporalValid` would latch on it as it does on an evaluate, and the status line would say "temporal inputs", which
is true.

**History.** A change of source (constant, observed, tags), or of the picked resource, extent or direction, is a
history event. It resets once after `kSourceStable` frames, the debounce present-real-guides.md D3 sets out for the same
reason. One case needs naming: a game that ping-pongs two identical depth buffers changes resource every frame by
design. Two resources of identical shape alternating are treated as one source, and step 0's "pick changed" count is
what shows it.

## Risks

- **Wrong buffer means visible damage.** Edges the model trusts that are not in the colour. The refusal rules, the
  margin and step 0's ground-truth score are the defence. A refusal costs a frame on constant depth; a wrong pick costs
  the picture. When in doubt, refuse.
- **Reversed-Z.** Direction is inferred from a full clear to exactly 0 or 1. A game that never clears depth (it
  overwrites with a full-screen pass), clears with rects, or uses an unobserved render pass gives no
  direction. A fully observed render-pass beginning clear to 0 or 1 must count like the explicit clear.
  - Step 0 counts each case and compares with the ground truth.
  - The PSO's depth comparison function would be a second source. It needs pipeline-state tracking (`SetPipelineState`
    is already hooked for root restore, `D3D12_Hooks.cpp:1091`), and is not proposed until step 0 shows the clear
    insufficient.
  - No linearization: the model takes device depth with an inverted flag, as from DLSS (`GatherGuideFrame`, `DlssNr_Dx12_Inputs.cpp:110`).
- **MSAA depth.** It cannot be copied to a single-sample clone, and an averaging resolve is meaningless for depth.
  Refused in v1; a resolve pass (min, max or sample 0) is a new shader with its own native-Windows check.
- **Typeless and planar depth.** The resource can be `R32_TYPELESS`, `R24G8_TYPELESS`, `R32G8X24_TYPELESS` or
  `R16_TYPELESS`, with the DSV giving the typed view. The copy keeps the resource's own format, and `ReadableGuide`
  makes the typed clone. D24S8 and D32S8 copy plane 0 only. nr-present-hook.md already lists these families as
  unproven on native Windows, and this adds no format that list lacks.
- **Placed and aliased resources.** Render-graph engines alias transient memory, so at present a placed depth's memory
  can hold a post-process target. Aliasing barriers are refused. Placed resources made through `CreatePlacedResource1/2`
  are not visible as placed today (only vtable 29 is hooked). Step 0 counts committed, placed and unknown placement,
  and the data decides whether those methods need hooks.
- **Barrier state unknown at present.** D3D12 has no query for a resource's state, and a depth-stencil texture never
  decays or promotes, so tracking is exact *if* every barrier is seen in execution order. It is not when a barrier ran
  on another queue, an enhanced barrier touched the pick, or a list predates the hooks. Each is a refusal, never a
  guess.
- **Cross-queue.** A pick written or transitioned on another queue is refused, never waited for: this tree's rule
  against cross-queue GPU waits (present-real-guides.md, D4) stands.
- **Hook cost.** Per-draw hooks are the expensive part, which is why R1 exists. Step 0 logs calls per frame and a
  sampled cost, and drops the draw hooks if R1 agrees with the ground truth as well as R2.
- **Two detours on one slot.** With `FGInput=upscaler` and the HUD fix, `ResTrack_Dx12::hkOMSetRenderTargets` and
  `hkCopyDescriptors*` are already attached. Detours stacks a second attach, but the combination needs a run (the test
  plan has one).
- **Lifetime.** Step 0 holds no reference to a game resource: entries are pointers with a description copied at DSV
  creation, dropped through `ID3DDestructionNotifier` as `ResTrack_dx12.cpp:166-203` does. Step 1 references only the
  pick, from recording its copy until the present list is known complete (`DlssNr_Dx12_Lifetime.cpp:436`, `:549`). A game
  may release its depth on resize while our copy is in flight; the reference covers that.

## The D3D11 bridge (Divinity, The Witcher 3 DX11)

There the depth lives on the game's D3D11 device, and the pass on the bridge's D3D12 device. The design differs because
D3D11 removes two of the D3D12 risks and adds one transport.

- **What goes away.** D3D11 has no barriers and one immediate context: the runtime (DXVK under Proton) tracks state and
  orders a copy after the draws itself. "State unknown" and "other queue" do not exist.
- **Observation.** D3D11 hooks that do not exist today:
  - `ID3D11Device::CreateDepthStencilView`;
  - `OMSetRenderTargets` and `OMSetRenderTargetsAndUnorderedAccessViews`;
  - `ClearDepthStencilView`, and the draw family;
  - `OMSetDepthStencilState`, so only depth-writing draws count, as NeuRotic's D3D11 path does
    (`NativeD3D11Guides.cpp:158-170`);
  - `ExecuteCommandList`, for deferred contexts.
- **Reuse before present.** Snapshot the pick at the clear that would destroy it, NeuRotic's D3D11 move
  (`NativeD3D11Guides.cpp:171-195`), with D3D11 ordering the copy.
- **Transport.** The bridge already moves the backbuffer through a shared texture and a shared fence
  (nr-dx11-bridge-host.md, "What is already here"):
  - `CopyResource` the depth into a typeless, SRV-capable twin, in the same format family (a typed depth format cannot
    have an SRV);
  - convert it to `R32_FLOAT` in a small D3D11 pass, into a shared texture;
  - signal on the same fence, so the D3D12 side waits as it does for the colour.

  That costs a new D3D11 shader (an fxc `_Shader_Dx11.h` header, rebuilt under the invariants) and one shared texture.
- **Risks.**
  - MSAA was common in D3D11-era renderers, and is refused.
  - Sharing an `R32_FLOAT` texture from DXVK to vkd3d-proton is untested, though the colour path shares fine in
    Divinity and The Witcher 3.
  - Deferred contexts must be followed or the pick refused.
- **Order.** After D3D12's step 1, as its own step with its own log-only slice. The two titles are slow-camera
  (Divinity) and fast-orbit (The Witcher 3). Generation Zero, where the zero-motion ghosts were seen (Rafael's
  Windows), is the third.

## Proton, vkd3d-proton and native Windows

- **Proton is not a D3D12 validator** (reduced-scale-guides.md, "Native Windows"; `13f10d60`).
  - Step 0 records no GPU command. Hook installation may need temporary objects to discover interface vtables.
    Native Windows must verify installation, exact forwarding and coexistence with existing detours as well as
    absence of a crash; host ranking tests do not establish those properties.
  - Step 1 transitions a game resource from a state we tracked. A wrong "before" state is accepted by vkd3d-proton, and
    is the kind of fault native Windows answers with corruption or removal. On AMD it can damage the game's own depth
    compression, harming the game and not only NR.
  - Step 1 therefore needs a native run with the debug layer **and GPU-based validation**, the only layer that checks
    resource states at execution: Rafael's RTX 3060.
- **Descriptor handles.** Under vkd3d-proton, CPU DSV handles are host pointers into its heaps. Games copy DSVs between
  heaps, which is why the descriptor-copy hooks are needed on both platforms.
- **Queues.** vkd3d-proton can put several D3D12 queues on one `VkQueue` (`OptiScaler/with_dx12/dx11_with_dx12.cpp:244`). It does
  not matter here, since nothing waits across queues.
- **D24S8.** Native on this machine's NVIDIA driver. On AMD, vkd3d-proton backs it with another format. A copy between
  two resources of the same D3D12 format stays consistent; this is noted, not tested.
- **`ID3DDestructionNotifier`** under vkd3d-proton: `ResTrack_dx12.cpp:183` already logs when it is missing.
  The original proposed raw-pointer fallback could mislabel a reused address even in the log. The proposed
  replacement is an `unobserved` refusal when generation/lifetime identity cannot be established; see the pending
  decision above. Never count that case as a confident ground-truth match.

## Inert when off, default-identical, config

**One key**, a new quantity (where depth comes from when no upscaler supplies it). It is independent of `SynthMotion`
(where motion comes from) and of `RequireDlss` (whether the upscaler's guides are required):

```
; D3D12 present diagnostic at HookMethod=2; D3D11 discovery is not implemented:
; constant: the depth guide is a constant, as always. log: watch the game's depth buffers and write to the
; log which one would be used and why; nothing is used (diagnostic). Read at start-up.
; constant or log - Default (auto) is constant
PresentDepth=auto
```

- `observed` is added in step 1. `log` stays as the diagnostic, so no value ever changes meaning (rule 3).
- Four points: `CustomOptional<std::string> DlssNrPresentDepth { "constant" }` in `Config.h`, read and saved in
  `Config.cpp`, the text above in `OptiScaler.ini`, and `Config.md`. `nr-invariants` checks the round trip for every
  `DlssNr` key.
- **Gate.** The observer's detours are installed at device creation only with `Enabled=true`, `HookMethod=2` and
  `PresentDepth=log`, all read at start-up. Unknown values must remain inert; `observed` is not accepted in step 0.
  Otherwise there is no detour, no allocation and no lock: the default is byte-identical, and `HookMethod=1` users, `HookMethod=2` users who did not ask, and the non-NR paths pay
  nothing.
- **Menu (rule 2).** Nothing in step 0. From step 1, one selector under the present controls, shown only with the
  present route active, `RequireDlss` off and a host that supports it, labelled "applies after a restart" (the pattern
  of nr-before-dlssg.md's boundary). Under it, a read-only line in use: "depth: the game's (3440x1440, reversed)" or
  "depth: constant (refused: state unknown)". With `RequireDlss` on it is hidden, because frames without the upscaler's
  guides are skipped and observed depth would never be read.
- **Frame hold** must freeze the depth copy with the colour (frame-hold.md): no new copy while held.

## Review against DEVELOPMENT.md

| Item | Anticipated answer |
|---|---|
| 1 Default-identical | `PresentDepth=constant` (the default): no detour installed, no allocation. Step 0's `log` changes no frame. |
| 2 No dead control | No control in step 0. In step 1, the selector appears only where observed depth can be read. |
| 3 One quantity, one control | One key for depth's source. Motion keeps `SynthMotion`, requirement keeps `RequireDlss`. No value changes meaning. |
| 4 Config round-trip | All four points with the key, in step 0. |
| 5 Struct equals cbuffer | No change in steps 0 to 2. |
| 6 Shader rebuild | None on D3D12. The D3D11 step adds one shader and its precompiled header to `nr-invariants`' freshness list. |
| 7 Passthrough | Untouched: depth is a guide, not the encode. |
| 8 Vulkan lifetime | No Vulkan change. |
| §3 Non-NR path | New detours on D3D12 device, list and queue methods, which upstream's paths also use. Reviewed for inertness at the gate, for stacking with ResTrack's HUD-fix detours, and for no lock held across a game call. |

## Steps

### Step 0: log only (D3D12)

The proposed observer, behind `PresentDepth=log`, ranks and refuses without handing anything to the pass.
The original requirement to use the identity Tracker's boundary count is **not yet implementable on the
no-upscaler arm**; the revalidation section specifies the proposed clock amendment. Step 0 is D3D12 only;
D3D11 remains step 2. If the title has an upscaler, it also
compares its pick with the upscaler's depth (`DLSS.Depth` / `NVSDK_NGX_Parameter_Depth`, already read by
`CaptureTemporal`, `DlssNr_Dx12_PresentGuides.cpp:848`) and its direction with the create flags.

**Proposed line per window** (the comparison fields await the amendments above), written by the windowing rule of the identity log (at least 2 s; held up to 30 s while
unchanged; the first window is always written; nothing when no depth target was ever bound). Counts made up:

```
DLSS-NR present depth, candidate scan (log only, nothing used), D3D12 present: 120 presents in 2.0 s, 0 upscaler captures | depth targets bound 5, 2 at the backbuffer's extent | would pick 112, last bound and most draws agree 110 | would refuse 8: none 2, extent 0, multisampled 0, tie 1, direction 0, state 3, cleared 2, aliased 0, other-queue 0, unobserved 0 | pick changed 1 | observer 6214 calls a frame, 0.09 ms a frame (sampled)
```

When the window had upscaler captures, a field follows the capture count:
`| the upscaler's depth: picked 110 of 112 eligible comparisons, direction agrees 110`.

**And one line per change of pick** (resource, extent, format, direction or state source):

```
DLSS-NR present depth, would pick 3440x1440 R32_TYPELESS (DSV D32_FLOAT), committed, 1 sample: cleared to 0.0 (reversed), state DEPTH_WRITE (barrier, present queue), 2412 draws while bound writable (runner-up 2048x2048 R32_TYPELESS at 640, not at the backbuffer's extent), last bound 0.4 ms before present; backbuffer 3440x1440
```

The fields:

- `presents`: presents that reached the pick in the window.
- `upscaler captures`: commits to the guide ring in the window. Zero is the title with no upscaler.
- `depth targets bound`: distinct DSV-backed resources bound writable in the window, and how many at the backbuffer's
  extent.
- `would pick`: presents where R1 found a candidate that passes every rule. `agree`: of those, how many R2 named the
  same resource.
- `would refuse`: by first failing reason, in this order:
  - `none`: nothing admissible;
  - `extent`;
  - `multisampled`;
  - `tie`: R2's runner-up above half;
  - `direction`: no full clear to 0 or 1;
  - `state`: never seen, or an enhanced barrier;
  - `cleared`: a clear after its last use;
  - `aliased`;
  - `other-queue`;
  - `unobserved`: a bundle, a list from before the hooks, a render pass the hooks could not follow.
- `pick changed`: changes of picked resource. Alternation between two identical buffers is counted once.
- `observer`: hooked calls per frame, and their cost from a 1-in-64 sample.
- `the upscaler's depth`: of presents with a pick and an eligible capture, how often the pick is the resource the
  upscaler was handed, and how often the inferred direction equals `DepthInverted`. A game that hands its upscaler a
  separate copy of depth reads as a miss here; the pick line's extent and format show it.

The string `candidate scan (log only` must be in the DLL once the slice is linked in. It is not delivered by
this documentation-only revalidation.

**How to read it for step 1:**

- **Precision.** In a DLAA arm, score matches against **eligible comparisons for each ranking**. Total ring
  commits are a separate coverage count, not the denominator. The original sample line cannot supply both
  rankings' precision and needs the proposed fields above. Below 99% in any title, step 1 does not go ahead
  for that ranking. Keep coverage and refusals beside precision so a nearly-always-refusing observer cannot
  appear successful from a tiny selected subset. A miss pattern (menus, cutscenes) is read from the pick lines.
- **R1 against R2.** If `agree` is close to `would pick` and R1's precision equals R2's, the draw hooks go. If R2 is
  clearly better, they stay, and their measured cost is the price.
- **Which risks are real.** `state`, `cleared`, `aliased` and `other-queue` say which engines need step 3 or more
  hooks; `multisampled` how often MSAA blocks the route; `direction` whether a second source is needed.
- **Cost.** `observer` against the frame time decides whether the hooks are acceptable at all.
- **The design's own test.** If, across a session that looks right, the target arm refuses most frames or the DLAA arm
  scores low, the heuristic is not good enough and this note is rewritten before step 1.

### Step 0b: is depth worth finding? (specified, not built; scratch builds never merged)

**Question isolated by A/B:** real motion with real depth versus the **same real motion** with constant depth.
This experiment does not test a depth observer and does not prove anything about its ranking. It also does not
answer whether synthesized motion plus real depth lifts the clamp; that needs separate arms after this pair.

**Build contents.** The integration owner builds two isolated scratch variants from one reviewed source ref,
with the same compiler, embedded shaders, config and model:

- **A, control:** unmodified present route. Only a guide pair accepted by the existing ring and identity checks
  is eligible for comparison. Record build stamp, source ref and SHA-256 of the deployed proxy and model.
- **B, depth ablation:** at the D3D12 present consumer only, **after** the same pair has passed identity and
  submission checks, replace the selected cloned depth with an owned, same-extent `R32_FLOAT` zero guide in
  its known readable state. Preserve the selected motion pointer, scale, encoding, subrect and capture identity,
  and the real frame's inversion flag. Keeping inversion unchanged isolates spatial depth content; a separate
  optional arm may test the exact fallback's `DepthInverted=false`, clearly labelled as changing two inputs.
  The depth's own metadata must describe its real size, origin and state. Refuse the experimental frame if the
  constant cannot be safely prepared; never label a silent real-depth fallback as B. Use the existing tracked
  present recording/retirement mechanism for the owned guide; no new private queue, CPU wait or borrowed
  resource lifetime. Clear only on initialization. With no accepted pair, run the ordinary path and count the
  frame as **excluded**, not a B sample.
- A and B must keep history, cadence, hold, HDR/colour, motion and reset policy identical. B must not take the
  ordinary zero-motion fallback or its forced `AllowSupersampling=false`, since either would confound the
  experiment. Add a scratch diagnostic that says A/B, eligible/excluded frames, guide extent, working extent,
  inversion, identity refusal and model acceptance. Do not log addresses or personal paths.
- The scratch patch must have its own source diff and binaries. No new production setting, shader, installer or
  distributed model is part of this task. The lead reviews/builds it under the shared-prefix rules; nothing in
  the present task ran the compiler or produced either binary.

**Owner's Crimson Desert procedure:**

1. Close the game. Select a fixed save, location, camera starting point and time/weather when possible. Record
   GPU, driver, OS/Proton, output resolution, game version, model SHA-256 and model/pass settings. Keep the game
   at **DLAA**, render scale 100%, dynamic resolution off, game DLSS-G off for the primary pair. Restart after
   changing those options. DLAA deliberately supplies real guides; it is not the no-upscaler arm.
2. Install A with the existing receipt-based installer while the game is closed. In the selected game's INI set
   `[DlssNr] Enabled=true`, `HookMethod=2`, `RequireDlss=true`, `WorkingScale=1.0`, `Passes=1`, `Cadence=1`,
   `UseProxy=false`, `Compare=0`, `DebugView=0`, `HoldFrame=false`. Keep every remaining key identical between
   arms and retain a sanitized copy with the private evidence. Leave `PresentDepth` at its default once that
   key exists: the scanner is not part of this ablation. No runtime hook-method toggle substitutes for startup.
3. Start the game and verify A's actual loaded proxy/build stamp. Wait for the selected scene and the model's
   warm-up. Check the recent guides line and scratch counters: only frames with an identity-accepted real pair
   are eligible. If the pass never receives such a pair, stop the experiment; the scene cannot answer this question.
4. Capture a stationary view using **Hold frame**, then **Capture original and processed**, with Compare and
   DebugView still off. Wait for the capture's saved result; keep both images and its provenance JSON. Release
   Hold frame before changing the camera. This is a spatial sample of one held frame, not temporal evidence.
5. Repeat a fixed-duration pan/orbit from the recorded starting view with Hold frame **off**. Keep a video or
   an authorized temporal capture plus logs, since one PNG cannot establish ghosting or stability. Repeat the
   same route through gameplay, inventory/menu and a cutscene. Keep the same settings and duration for B.
6. Exit completely. Install B through the same receipt mechanism, verify its different binary hash and stamp,
   and repeat steps 3-5 from the same save and view. Do not overwrite a DLL while the game is running. Repeat
   A once more to distinguish a reproducible difference from scene variation; label the sequence A/B/A.
7. With the game closed between installations, repeat the paired sequence at `WorkingScale=1.5`. The scratch
   diagnostic must show the **actual** working extent and model acceptance/refusal in each arm: a control that
   was silently clamped to native is not a supersampling result. If the 1.0 pair already fails eligibility, do
   not reinterpret the 1.5 failure as evidence about depth.
8. Compare like-for-like originals first, then processed edges, disocclusions, fine detail, text/HUD, and the pan's
   temporal behavior. Record excluded/eligible counts, acceptance/refusal, CPU/GPU timing with their stream
   names, and any resets; do not report FPS alone as image-quality evidence. Nonidentical game frames limit
   visual inference even at the same save. Retain the raw captures/logs privately, outside Git.
9. Restore the known-good receipt/build while the game is closed. Record the restored hash and any unresolved
   difference. Native Windows and Proton results are separate observations.

A repeatable visible benefit from real depth supports continuing the scanner experiment, not using its picks.
If the two arms cannot be distinguished, record **no demonstrated benefit in these scenes**; that is not proof
that depth never matters. Step 1 still waits for valid step-0 precision/coverage numbers in two DLAA titles and
acceptable hook cost. Real-depth/synthesized-motion and constant-depth/synthesized-motion arms are later, separate
experiments; the A/B above cannot answer that interaction.

### Step 0c: the tag probe (partly superseded, log only)

Since the original design, [sl-tag-inventory.md](sl-tag-inventory.md) implements note-only tag inventory with NR
on even outside `FGInput=dlssg|nvngx`. Do not install a competing second tag observer. That inventory does not
establish state, queue ownership, frame identity or inversion constants. A separately authorized extension would
count per window the depth and motion tags (lifecycle, declared state, extent, format),
HUD-less tags, the constants (depth inverted, motion scale) and whether an upscaler evaluate also ran:

```
DLSS-NR present depth, Streamline tags (log only): 120 presents | depth tagged 120 (eValidUntilPresent, DEPTH_WRITE, 3440x1440 R32_TYPELESS), motion tagged 120 (eValidUntilPresent, 3440x1440 R16G16_FLOAT), HUD-less tagged 0 | constants 120: depth inverted, motion scale 1/3440 x 1/1440 | upscaler evaluates 0
```

This also answers nr-before-dlssg.md's open question 4 (whether Crimson Desert tags a HUD-less colour).

### Step 1: use it (D3D12 present)

`PresentDepth=observed`. It goes ahead only if step 0b shows depth matters, and step 0 scores at least 99% in the DLAA
arm of two titles at an acceptable cost.

- **Tags first.** Where step 0c finds them, tagged depth and motion go into the existing ring.
- **Otherwise (a).** The observed pick is copied at present, under every refusal rule.
- **Unchanged:** `AllowSupersampling` stays clamped until step 0b says otherwise, synthesized motion is unchanged, and
  so is `ZeroGuideReset`'s keying on motion.

### Step 2: the D3D11 bridge

Its own log-only slice, then use, as in "The D3D11 bridge".

### Step 3, if earned

- **Copy at the bind point**, into a second ring, for titles where step 0's `cleared` or `aliased` dominate.
- **A depth term in the motion estimator's expand pass** (synthesized-motion.md, "Static overlays"): choosing between
  neighbouring block vectors along depth edges. Measured in `synth-motion-d3d12` before any game.
- **No camera reprojection:** without the game's matrices there is nothing to reproject with, and only (b1) supplies
  them.

## Test plan

**Host tier.**

- **New: `tests/nr-present-depth`**, registered in `tests/suites.toml` and indexed in `tests/README.md`. It builds a
  standard-library decision header, `retoque/dlssnr/DlssNr_PresentDepth.h` (the pattern of
  `DlssNr_PresentGuideIdentity.h`), which the hooks feed and both hosts ask. Event streams:
  - **Candidates against the scene depth:** a shadow-cascade array beside the scene depth; a first-person pass with its
    own full-size depth; an inventory 3D preview bound last during a menu; two identical buffers alternating every
    frame; a depth-only copy pass.
  - **Every refusal alone:** cleared after use, an aliasing barrier, a render pass clear, an enhanced barrier, MSAA,
    the wrong extent, clears to 0.5 and with rects.
  - **State:** from creation, from barriers, with a mismatched "before".
  - **Queues and recording:** a barrier on another queue; four recording threads with out-of-order submission; a list
    recorded before install; a bundle; our own lists and resources excluded.
  - **Margins and history:** R1 against R2, the 2:1 margin, the ground-truth counters, `pick changed` and the
    alternation rule.
  - **The log:** the window, both lines and their fields.
  - **Source guards:** in step 0 the observed resource never reaches `EvaluateAtPresent`, and the detours are installed
    only under the gate.
- **`nr-invariants`**: the key's four points (automatic), and its value set equal in `Config.cpp`, the ini and
  `Config.md`.
- **Unchanged, as regressions:** `nr-present-guides`, `nr-present-guide-identity`, `nr-present-boundary`,
  `nr-present-host`, `nr-zero-guides`, `bridge-lifetime`, `repo-privacy`.
- **What these cannot show:** that a real engine's stream looks like the fakes, that the copy is legal on a real
  driver, or what the model does with the depth.

**Wine tier (proposed for step 1).** `present-depth-d3d12`: a small D3D12 program built through the msvc-wine prefix.
It draws a known scene depth, a shadow map and a UI pass, presents through the hooks, and asserts the pick, the
direction and a readback of the copy. Serial, like the rest of the tier.

**In-game.** One session per row, with build stamp, DLL hashes, Proton and driver versions and the raw log kept:

| Title | What it checks |
|---|---|
| Crimson Desert, **DLAA**, `HookMethod=2`, `PresentDepth=log` | The ground-truth arm: `the upscaler's depth` precision and direction agreement through gameplay, a menu, a cutscene, a resolution change and Alt-Tab; the observer's cost against `PresentDepth=constant`. |
| Crimson Desert, **no upscaler**, DLSS-G off, `HookMethod=2`, `PresentDepth=log` | The target arm: `0 upscaler captures`, the refusal mix, pick stability, cost. |
| Crimson Desert, no upscaler, **DLSS-G on** (if the menu allows it) | The interval at the boundary (one window per game present, not per DLSS-G present); step 0c's tag line. |
| Cyberpunk 2077, DLAA then TAA with no upscaler | A second engine for both arms; Ray Reconstruction off. |
| PCSX2 (D3D12 present) | An emulator's depth use: expect `extent` or `tie`, never a confident wrong pick; the observer's cost. |
| The Witcher 3 DX11, Divinity | Until step 2: nothing installed on the D3D11 device, no line. |
| DOOM Eternal (Vulkan) | Unchanged: nothing runs. |
| Any title at `HookMethod=1`, or `PresentDepth=constant` | No detour, no line. |
| Native Windows (Rafael's RTX 3060) | Step 0: the hooks do not crash, the line appears. Step 1: the debug layer with GPU-based validation over the copy and its transitions. |

**How to make Crimson Desert call no upscaler.**

- In the game's graphics options, choose the anti-aliasing mode that runs at full resolution with no upscaler: the one
  that is neither DLSS nor DLAA, FSR or XeSS (record its name with the run). Keep any render scale at 100%.
- **Not DLAA**: it is a DLSS evaluate at native size, which is exactly why it is the ground-truth arm.
- **Not FSR or XeSS**: OptiScaler intercepts those and normalizes them into the same evaluate (no-upscaler-titles.md).
- Confirm from the log, not from the menu:
  - the identity line (`identity check (log only`, `identity applied` since present-real-guides.md's step 1) never
    appears, since a ring that never recorded logs nothing;
  - inspect the recent guides line for zero/synthesized fallback. `HookStatus` (`DlssNr_Dx12.cpp:4412`)
    gives the own-Present boundary precedence, so its route text alone is not proof of guide provenance;
    routing still uses a latch, so restart after changing the game's upscaler setting;
  - the scan line shows `0 upscaler captures` in every window.
- `HookMethod=2` must be in the ini before start-up, both for the boundary under DLSS-G and for the observer's gate.
- If the game offers no such mode, Crimson Desert is the DLAA arm only, and Cyberpunk 2077 carries the target arm.

## Open questions

1. **Is depth worth it?** Step 0b answers, and nothing past step 0 should be built before it does.
2. **Does real depth with synthesized motion lift the native clamp above `WorkingScale=1`?** Still open: step 0b's 1.5 A/B uses real motion, so separate synthesized-motion arms are needed.
3. **R1 or R2?** Is "last full-size depth bound" enough, or are per-draw hooks needed, and does any engine need the
   PSO's depth state?
4. **Does Crimson Desert offer DLSS-G without an upscaler, and does it still tag depth and motion then?** That decides
   whether (b1) has a test title on this machine.
5. **How common are scene depths cleared or aliased before present** (render-graph engines), that is, is step 3 needed?
6. **What observer cost is acceptable**, in calls and milliseconds a frame, and measured on which CPU besides this
   workstation's (Rafael's PC is the slower one we can reach)?
7. **The key is decided for step 0 (2026-11-19):** `PresentDepth=log`, default `constant`/`auto`, plus
   `Enabled` and `HookMethod=2`. No key or hook is implemented by this revalidation. `observed` remains future work.
8. **A depth smaller than the backbuffer** (a game's own spatial upscaler or dynamic resolution): refused here. Should
   a proportional extent be accepted as the ordinary render-size guide contract (reduced-scale-guides.md)?
9. **A user override for direction**, as NeuRotic has: a control that is right only for some titles, and dead when the
   clear already says. Not proposed; is it wanted?
10. **Placed resources made through `CreatePlacedResource1/2`**: hook them too, or rely on aliasing barriers alone?
11. **The HUD at scene depth.** On a present frame the interface sits over scene depth it does not belong to. Constant
    depth has no edges there; real depth does. Does that change how the model treats text, with or without
    `UiProtection`?
