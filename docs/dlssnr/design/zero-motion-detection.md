# Zero-motion detection: game motion vectors that read all zero in a moving scene

Status: **implemented on D3D12** (after-upscale, the D3D12 present route's captured guides, the D3D11 bridge's native
DLSS guides: every route that reaches `DlssNr_Dx12::Dispatch` with the game's own guides). **Not on Vulkan.** No
new ini key. Host-tested (`tests/nr-zero-motion`); **not yet run in a game**.

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

### Not on Vulkan

The Vulkan after-upscale route (`EvaluateAfterUpscaleVk`) consumes the game's vectors as well, and is not covered.
It needs the same shader as SPIR-V, a pass in the shape of `DlssNr_GuideMatch_Vk` (drained before free, abandoned with
a lost device, rule 8) and a readback the route can poll without a wait. `DlssNr_MotionProbe.h` is written so that pass
would only have to produce the same grid. The Vulkan present host runs on zero guides or its own estimate, never on
the game's vectors, so it has nothing to detect.

## Cost

One dispatch of 16 groups and a 16 KB copy every eighth evaluate, on the pass's own list; a 16 KB scan on the CPU
when a slot completes. Next to the model it is not measurable; `GpuTiming` would include it in the pass's sample.

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
| Device lost / removed | The pass's slots are never read again; it is parked with the rest. |

## Test

`tests/nr-zero-motion` compiles `DlssNr_MotionProbe.h` under ASan/UBSan and drives it with synthetic grids: jitter on
a sloped plane (no verdict), a pan (verdict after two readings, not one), a still scene, a partial move, NaN motion,
a single non-zero sample clearing it, a shape change and an old probe starting it over, the reset rule against every
input, the log lines once each. `run.py` holds the shader's constants, registers and grid to the header's, the
bytecode optional, the probe recorded in one place after the guides are readable, the reset rule wired where the
frame's reset is read, the stabilizer reading the policy flag, the pass parked and freed, and the files in the project.

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
