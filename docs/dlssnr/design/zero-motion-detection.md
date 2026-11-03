# Zero-motion detection: game motion vectors that read all zero in a moving scene

Status: **implemented on D3D12** (after-upscale, the D3D12 present route's captured guides, the D3D11 bridge's native
DLSS guides: every route that reaches `DlssNr_Dx12::Dispatch` with the game's own guides), and **on the Vulkan
after-upscale route** since 2026-11-03 (section "Vulkan" below). No new ini key. Host-tested (`tests/nr-zero-motion`,
`tests/nr-vk-zero-motion`); **not yet run on a GPU or in a game** on either API.

## Why

The model reprojects its history along the motion vectors it is handed. Handed an all-zero field while the camera
moves, it believes nothing moved and blends each frame with where things used to be: ghosts and doubled edges, the
artefact `ZeroGuideReset` was written for on the zero-guide routes (Generation Zero, 2026-09-24,
[synthesized-motion.md](synthesized-motion.md)). The same thing happens with *the game's own* guides if the motion
texture it hands over is never written: the pass treats it as real motion, `ZeroGuideReset` does not apply ("no effect
when real guides are captured"), and nothing says anything.

### Evidence

- **neural-amd v0.7.15** (`tmp/neural-amd`, MIT, `8a8144d`, 2026-10-23): a game motion buffer (Assetto Corsa with
  Custom Shaders Patch) and `AMD_Neural_Feed.fx`'s field (issue #18) both arrived all zero while the scene moved, and
  "fed as fact, an empty field smeared the history (flashing in Assetto Corsa with Custom Shaders Patch)". Its probe
  (`core/addon/probes.inc:146-219`) reads the motion and depth back on the CPU, counts exactly-zero texels, and gives
  the buffer up after **two readings of a moving scene** -- a reading counts only when the depth read with it changed,
  because "a still menu reads all zero honestly". It then takes motion from optical flow or its estimator, and retries
  the effect's field later, doubling the wait.
- **Here, nothing has been observed yet.** Every route this fork runs on today takes its guides from what the game
  handed an upscaler, and an upscaler fed a dead motion field ghosts too, so a shipped title is unlikely to do it on
  its main path. The cases that remain plausible are ours or the game's off-path ones: a present-time capture that
  picked the wrong buffer, a D3D11 transport copied from a texture the game stopped writing, a cutscene or photo mode
  that keeps calling DLSS with a cleared motion target, a game that writes no motion for one camera mode. So this is
  a guard with a log line first, and a fix second. The log line is what will say whether it ever fires.

## What counts as dead

Two **readings** in a row, each of which is:

1. **all zero**: every motion sample in the probe is exactly `(0, 0)`. A NaN or an infinity is *not* zero (that is
   a different fault, and calling it dead would reset history over it); any non-zero sample anywhere clears it; and
2. **a moving scene**: compared with the previous probe, at least half the depth samples that can tell (see below)
   changed by more than jitter can explain.

and it stops being dead at the first probe with a single non-zero motion sample, or when what is probed changes
shape (render size, guide region, display/render-resolution vectors): a different buffer is judged afresh.

### Why depth has to move, and how much

A still scene has zero motion *and* still depth; a menu over a frozen frame too. Those are honest zeros, and must
never count. neural-amd compares the sum of depth exactly; that does not survive TAA jitter, which moves every sample
of a sloped surface by a fraction of a pixel each frame while the camera stands still and the game's vectors are,
correctly, zero (DLSS takes vectors without jitter). So per sample the probe also measures the depth's largest step to
a direct neighbour (`g`, one pixel's worth of depth there), and a sample has **moved** only when

    |d_now - d_before| > 2 * max(g_now, g_before)   and   > 1e-4 * max(|d_now|, |d_before|)

-- more than two pixels' worth of the local depth slope, which sub-pixel jitter cannot produce, and above float
noise. Silhouettes (huge `g`) never count; they do not need to, the interior of every surface does. Samples whose
depth is identical in both probes with a flat neighbourhood in both (sky, a far-plane clear, a wall square to a still
camera) are left out of the count altogether: they cannot tell. At least 64 of the 1024 samples must be able to tell,
and half of those must have moved.

Probes are **eight evaluates apart**, so a slow pan has eight frames to cover its two pixels, and the comparison is
dropped when the previous probe is more than four intervals old (the route paused, or skipped frames in between).

Half the samples is deliberately conservative. A camera that moves changes nearly every non-sky sample; what changes
a smaller part of the frame while the camera stands still -- a character, animated water or foliage the game writes
no motion for -- is exactly what must not count, because resetting history there costs shimmer for nothing.

## What it does

The verdict is applied as `ZeroGuideReset` applies to a zero-guide frame: **while it holds and `ZeroGuideReset` is
on, every frame with the game's guides drops the model's history**, marked as policy (`ResetIsPolicy`), so the output
stabilizer and the log do not read it as a cut. A reset the game asked for that frame is left as the game's.

With `ZeroGuideReset` off (its default) nothing on screen changes, by invariant 1 (default-identical), and **the probe
does not run at all**: it is a D3D12 pass of its own, every eighth evaluate, and no game of ours has shown dead vectors,
so it is not worth a pass on every NR session before it has run on native Windows (2026-10-29). The first version
probed always and only logged with the key off. The choice is the same trade the key already is -- no ghosts against three
times the shimmer of the raw game on a still background (the PCSX2 measurement in synthesized-motion.md) -- and that
trade was made opt-in for zero guides; a dead field is zero guides by another name.

Not done here, and why:

- **Depth is kept.** It is real (the probe saw it move); only the motion is dead.
- **No switch to the motion estimator** (`SynthMotion`). That is neural-amd's answer and the better one, but on this
  fork the estimator is wired into the present hosts' zero-guide branch, before `Dispatch`, with its own frame
  description and zero depth; routing a dead game field there is a change to each host. A follow-up, once the
  detector has been seen to fire.
- **No new ini key.** If the detector ever misfires in a game, the escape is `ZeroGuideReset=false`, which leaves
  the detector logging only.

## Where

`DlssNr_Dx12::Dispatch`, the one function every D3D12 route reaches with `frame.GameGuides`, at the point where
depth and motion have just been made readable (`ReadableGuide`, the typed clones for typeless guides) and before the
guide resample: the probe reads the **game's region** of each (the subrects the pass already computed), not the
resampled copies. Frames without the game's guides (zero guides, synthesized motion) are not probed and not affected.

- **The GPU half**: `precompile/dlssnr_motion_probe.hlsl` (`DlssNr_MotionProbe_Dx12`): one dispatch of 4x4 groups of
  8x8 threads, one thread per cell of a 32x32 grid over the region. Per cell: the depth at its centre, that depth's
  largest step to its four neighbours, and how many of a 4x4 lattice of motion samples are not exactly zero, as one
  RGBA32F texel. 1024 depth reads (x5 for the neighbours) and 16384 motion reads, every eighth evaluate. The grid is
  copied into one of four readback buffers (16 KB each).
- **The CPU half**: `retoque/dlssnr/DlssNr_MotionProbe.h`, pure and host-tested: a grid into a probe, two probes into a
  reading, readings into the verdict, the shape that starts it over, the reset rule and the log text.
- **Completion**: each slot's recording is tracked (`DlssNr::Submission`), and a slot is read only once its fence says
  the copy finished, as the pulse metric does; never a CPU wait. The verdict therefore lags the GPU by the game's
  frames in flight, plus up to one interval.
  A slot whose list was dropped without executing (a present list the pass gave up on) never completes; once the
  tracker calls it settled it is freed unread, so dropped lists cannot use up the four slots.
- **Lifetime**: the pass is built on the first probe of a device, rebuilt with a new device, parked through the
  retired list like every other diagnostics pass, and deleted at shutdown.
- **Inert when NR is off**: it lives inside `Dispatch`, which nothing calls with `DlssNrEnabled` off.
- **Optional bytecode**: the pass includes its header through `__has_include`; without it the probe reports itself
  unavailable once and the verdict never forms.

### Vulkan

The Vulkan after-upscale route (`EvaluateAfterUpscaleVk`) hands the model the game's own depth and motion, and probes
them the same way (2026-11-03). The Vulkan present host is not covered and needs nothing: it runs on zero guides or its
own estimate, never on the game's vectors, and `ZeroGuideReset` already applies to it. Vulkan backends bridged to D3D12
(`*_on12`) reach the D3D12 probe through `IFeature_VkwDx12`.

What is the same: the grid, the verdict and its log lines (`DlssNr_MotionProbe.h`, unchanged, and its `EventLine` text,
so the in-game checks below grep the same lines on both APIs), the trigger and the escape (`ZeroGuideReset`), and the
effect: while the verdict acts, every evaluate drops the model's history (`g_vk.reset`) unless the game asked for a
reset that frame, counted and logged on its own ("DLSS-NR Vulkan: model history dropped on the game's all-zero motion
vectors ..."), never in the game's reset counter. The Vulkan route has no output stabilizer, so there is no policy flag
to carry.

- **The GPU half**: the same `dlssnr_motion_probe.hlsl` built with `-D VK_MODE` into `DlssNr_MotionProbe_Shader_Vk.spv`
  and its header (the command at the top of the shader). The `VK_MODE` block declares the eight constants as a 32-byte
  push-constant block, the game's depth and motion as sampled images at bindings 0 and 1, the grid as an `rgba32f`
  storage image at 2; the body and the D3D12 declarations are textually unchanged, and the D3D12 DXIL is the same bytes
  (rebuilt with the repository's dxc: `ca055e90...` before and after). The pass is
  `retoque/shaders/dlssnr/DlssNr_MotionProbe_Vk.h/.cpp`, in the shape of `DlssNr_GuideMatch_Vk`: push descriptors (it
  needs `VK_KHR_push_descriptor`, which NR's device hook adds), a plain handle bundle, `Destroy` after proof, `Abandon`
  with a lost device. Its SPIR-V header is optional, as the D3D12 one is.
- **Where**: decided before the frame's first command, after the guide decision; reserved after the generation pin and
  the meter's reservation, before `BeginRouteRecording` (the V-06 order, [vulkan-recording-lifetime.md](vulkan-recording-lifetime.md));
  recorded after the exposure meter and before the guide resample and the model, on the game's regions. The game's
  views are bound in `SHADER_READ_ONLY_OPTIMAL`, as the guide resample binds them, and never transitioned.
- **Four slots, each exclusive to one recording.** A slot is a 32x32 RGBA32F grid and a 16 KB host-visible, coherent
  buffer (cached where the device has such a type). The recording that reserves a slot pins it until the recording
  registry sees that command buffer reset, re-begun or freed (`VkMeterReadback::Ring`, the exposure meter's contract);
  submission and fences are never read. Every recording starts its grid from `UNDEFINED`, so a slot passing between queue
  families needs no ownership transfer (an exclusive resource taken over that way has undefined contents, and nothing
  reads them before the dispatch writes every texel). A recording is given a slot only when its `Begin` was observed
  without `VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT`: Vulkan then forbids a second execution while one is pending,
  so the slot's grid has one writer at a time. The registry learns those flags from its three `Begin` observers
  (`VkRecording::Reuse`); a recording begun with the bit, or whose flags were not passed on, is refused, counted and said
  once per reason, and no probe is recorded into it.
- **The sentinel.** At reservation the slot's buffer is zeroed: no cell then carries its 16 motion samples, so a
  recording that retires without executing (discarded unsubmitted, or its encode failed) reads as no probe rather than
  as the grid an earlier recording left in that buffer. This replaces D3D12's "settled without completing", which Vulkan
  cannot observe.
- **Order.** Retired slots are fed to the detector by the order they were recorded (the ring's serial), whatever order
  the game resets its command buffers in; a slot recorded before the newest one already fed is freed unread.
- **Authority** (`retoque/dlssnr/DlssNr_VkMotionProbe.h`, the caller's state; `DlssNr_MotionProbe.h` is unchanged). The
  verdict acts only on an evaluate the probe observes, on the device its probes were read back on, while its newest
  probe was recorded no more than 64 evaluates earlier. It is dropped, and must form again from new probes, when the
  device changes or is lost (`ForgetMotionProbe`), when the route frees its objects (a hand-back, teardown), when the
  probe cannot observe the frames (no bytecode, the pass or its slots not made on the device, the game's views or
  regions refused, a refused recording), when no probe has been read back for those 64 evaluates (every slot held by
  command buffers the game keeps), and when `ZeroGuideReset` is switched on again after being off. Every probe recorded
  before the drop is discarded when its recording retires (its slot stays held until then), so the verdict forms again
  only from probes reserved after it. The drop is said when a verdict held. D3D12 keeps its verdict across a device change; Vulkan does not, so a probe that cannot be
  built on a new device can never leave resets running on evidence from the old one.
- **Default.** With `ZeroGuideReset` off (its default) the route reads the key and nothing else: no object is made, no
  slot reserved, nothing pinned, recorded or logged, `g_vk.reset` untouched; the probe's three ends (forget, abandon,
  destroy), called with the route's own, find nothing to do. NR off returns before any of it.

Limits, declared:

- **Re-execution of one recording.** A serial recording submitted again after its first execution completed rewrites
  its own slot; what is read once the game resets it is the last execution's grid, attributed to the evaluate the probe
  was recorded at. The comparison then spans another interval than assumed; the verdict still needs two moving readings.
  Simultaneous re-execution is refused only as far as the `Begin` flags say: a layer that begins command buffers
  without passing through the registry's observers leaves the recording unknown, which refuses the probe (and the route
  has no ticket for it either).
- **The game's layouts.** Binding the game's depth and motion as `SHADER_READ_ONLY_OPTIMAL` at native scale extends the
  guide resample's reliance on DLSS's read-state convention to every probed frame of a `ZeroGuideReset` user. No engine's
  barrier was inspected; a strict driver could read undefined data where an engine leaves depth in
  `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. A GPU harness with validation layers does not prove an engine's barrier either.
- **Retention.** A game that keeps its command buffers more than about 32 evaluates before resetting them holds all four
  slots: probes are skipped (said once) and, after 64 evaluates without a readback, the verdict is dropped. No verdict
  forms in such a game. More slots would cost 32 KB each.
- **Lag.** The verdict acts on probes whose command buffers the game has already reset: its frames in flight plus its
  reuse distance, plus up to one interval; two readings take at least sixteen evaluates after the baseline.
- **Sequence is not time** (both APIs): `seq` counts the evaluates the probe was asked about, so a pause with no
  evaluates is not seen by the four-interval gap rule. On Vulkan the switch of the key and the device/route boundaries
  start the verdict over; a long pause on one device with the key on does not.
- **Float folding.** The NaN test is `OpFOrdEqual` under `OpLogicalNot` in the SPIR-V; a driver compiling with relaxed
  float semantics could fold it. Only a GPU run can show it.
- **Not validated on a GPU.** `tests/nr-vk-zero-motion` drives the production route state, the production pass against
  recording fakes and the route's probe functions extracted verbatim, on the host; `spirv-val` accepted the SPIR-V. No
  device has run the shader, the barriers or a readback, under Proton or on native Windows, and no game has been probed.
  A GPU harness in the shape of `tests/guide-match-vk` (synthetic depth and motion in D32/R32/RG16F/RGBA16F, the grid
  read back against `Centre`/`TapAt`, the validation layer's synchronization checks) is the next step, then the in-game
  checks below on a Vulkan after-upscale title.

## Cost

One dispatch of 16 groups and a 16 KB copy every eighth evaluate, on the pass's own list; a 16 KB scan on the CPU
when a slot completes. Next to the model it is not measurable; `GpuTiming` would include it in the pass's sample.
On Vulkan the same dispatch and copy, in the game's command buffer between the route's timestamps; per device, with the
key on, four 16 KB grids and four 16 KB buffers (before allocation granularity), and up to four 16 KB reads in one
evaluate when retirements arrive together. Not measured.

## Failure modes

| Case | What happens |
|---|---|
| Still camera, TAA jitter, correct zero vectors | Depth moves by less than two pixels of slope: not a moving scene, no verdict. |
| Still camera, animated water/foliage with no motion written | Under half the samples move: no verdict. A full-screen water view could reach it; the reset then costs shimmer while it lasts, only with `ZeroGuideReset` on. |
| Menu or pause over a frozen frame, game still calling DLSS | Depth still: no verdict. |
| Game writes motion only for moving objects, camera static | Depth still where the camera is: no verdict. |
| Field cleared to a constant that is not zero | Not detected (only exact zeros). |
| Field of NaNs | Not detected (not zero); a different fault. |
| Real motion that is exactly zero at every probed sample while depth moves | Not possible for a camera move; a dolly along the view axis toward a fronto-parallel wall would still move depth and give the wall non-zero vectors away from the centre. |
| Resolution change, guide region change | Readings and verdict start over; the first comparison waits for two probes of the new shape. |
| Verdict held, game starts writing vectors | The next probe with one non-zero sample clears it and logs that it did. |
| Device lost / removed | The pass's slots are never read again; it is parked with the rest. On Vulkan the verdict is dropped with the device. |

## Test

`tests/nr-zero-motion` compiles `DlssNr_MotionProbe.h` under ASan/UBSan and drives it with synthetic grids: jitter on
a sloped plane (no verdict), a pan (verdict after two readings, not one), a still scene, a partial move, NaN motion,
a single non-zero sample clearing it, a shape change and an old probe starting it over, the reset rule against every
input, the log lines once each. `run.py` holds the shader's constants, registers and grid to the header's, the
bytecode optional, the probe recorded in one place after the guides are readable, the reset rule wired where the
frame's reset is read, the stabilizer reading the policy flag, the pass parked and freed, and the files in the project.

`tests/nr-vk-zero-motion` holds the Vulkan route: the push block equal to `ProbeParams` and the D3D12 cbuffer, the
bindings, the optional SPIR-V and the D3D12 declarations unchanged; every probe step behind `ZeroGuideReset`, in the V-06
order, with no CPU wait, and the three ends where the route ends its objects; then, compiled under ASan/UBSan, the
production `DlssNr_VkMotionProbe.h` with the real ring, registry and detector, the production pass against recording
Vulkan fakes, and the route's probe functions extracted verbatim. Cases: the command stream of one probe; a baseline
and two pans giving None, None, Dead, then Alive on one non-zero sample; the sentinel over a slot whose buffer holds an
old valid grid; out-of-order retirement; saturation, resubmission and staleness; Dead on device A then a stable failure
on device B with no policy reset; a refusal dropping the verdict; refused recordings; two live recordings writing two
grids; the shape fed being the shape reserved; forgetting with live pins; a drained teardown; creation failures; and the
three ends doing nothing with the key never on. Two more cases from the code review: probes recorded before the verdict was dropped,
retiring after it, are freed unread (the verdict forms again only from probes reserved after the drop); and a failed
layout or pipeline creation, whose output Vulkan leaves undefined, destroys only what was created. Each of ten planted
violations must fail a named case, and each of seven more a source guard.

What it does not cover: the shader on a device, a real readback, or a game. **In-game checks**, none run yet:

1. A title on the D3D12 after-upscale route (Crimson Desert), camera standing still and then panning, `LogLevel=2`:
   no `DLSS-NR motion probe:` line at all. This is the false-positive check, and the one that matters most: jitter,
   foliage, water and a still camera must never produce the verdict.
2. The same on the D3D12 present route (`HookMethod=2`) and on the D3D11 bridge with a native D3D11 DLSS title:
   no verdict.
3. A dead field on purpose, to see the positive path end to end: a debug build that hands the probe a cleared
   texture instead of `motionIn` (there is no ini switch for this, deliberately) gives the verdict line within about
   half a second of moving the camera and, with `ZeroGuideReset=true`, the `all-zero motion vectors` policy-reset
   line; moving the real field back gives `carry motion again`.
4. A run on native Windows (Rafael's PC) before calling it done: one new pass, one new constant-buffer view
   (256 bytes, `nr-invariants` checks it), one new readback; vkd3d-proton is not a D3D12 validator.
5. Vulkan: checks 1 and 3 on a Vulkan after-upscale title with `ZeroGuideReset=true` (the same `DLSS-NR motion probe:`
   lines, and `DLSS-NR Vulkan: model history dropped on the game's all-zero motion vectors` for the policy reset), with
   the validation layer on once; and the log's `DLSS-NR Vulkan motion probe:` lines, which say whether the probe came up
   and every reason it could not observe the frames.
