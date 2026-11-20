# A scene cut on unchanged content: FidelityFX's scene-change sums, 2026-11-20

RTX 4090, NVIDIA 620.73.04, Arch Linux (kernel 7.3.5-zen), Wine 11.22, Vulkan validation layers 1.4.363.1, base
tree `f515c36d`. An investigation of one intermittent failure of `tests/synth-motion-vk`, which ended in the shipped
estimator, not in the fixture. Native Windows, AMD and Intel hardware, games and image quality are outside this
evidence.

## What was seen

A whole Wine-tier run failed once in `synth-motion-vk`:
`time_3440: SceneCut() raised at [22, 24] with no cut in the content`. Everything else in that run was normal, three
reruns of the suite passed, and so had two earlier whole-tier runs. `time_3440` runs three frames in flight, so the
frames whose scene-change bit the GPU had set were 19 and 21.

## What it is

The scene-change value is FidelityFX's (`ffx_opticalflow_compute_scd_divergence.h`): each of 27 thread groups
normalises a 256-bin luma histogram and sums its divergence from the previous frame's. Both sums are tree
reductions over 256 threads. Down to 64 partial sums every step has a group barrier; the last steps (32, 16, 8, 4, 2,
1) had none, on the assumption that the 32 threads of one wave run each statement together. On this GPU they do
not. Thread 0 reads its neighbours' partial sums before they are complete, so:

- the histogram is normalised by a part of its mass, never by all of it. The previous histogram the pass stores
  should sum to 1; on every frame of every sequence it summed to between 3.1 and 4.8 (3.2 on most content) in one
  state of the GPU and to between 5.0 and 5.5 in the other. Those are what 10 and 6 of the 32 partial sums give for
  an evenly spread histogram (32/10, 32/6):
  threads 0 and 1 running together ahead of the rest, or thread 0 alone. That reading of the two numbers is an
  inference from them, not something observed inside the GPU;
- the divergence is a partial sum too, so the value is not FidelityFX's formula. Against that formula evaluated on
  the host from the same frames, the shipped pass was outside 1% on 273 of the suite's 294 judged frames (a median
  of 1.9 times the formula's value, up to 18 times; too high on 95% of them);
- which of the two ways the threads run is the GPU's scheduling, and it changes. A frame normalised one way,
  compared with a previous histogram normalised the other way, differs from it by a constant factor of about 1.6 in
  every bin, which the divergence reads as a scene change: 0.36, 0.45, 0.80, 0.85 on unchanged content, against 0.0001
  to 0.003 on the same frames otherwise, with a threshold of 0.45. The next frame is normal again. The failing run
  kept no such evidence, but its two flags are what two changes of scheduling two frames apart produce.

The fixture fed the estimator what it claimed. On the frames with a wrong value the colour read back from the GPU
after the estimator had read it hashed equal to the frame uploaded, the tickets and completion order were the
expected ones, the estimator's read-back slots held what the GPU's own output held, and the motion field of the same
frames was exact (EPE 0.000).

## Reproduction, rates and what load does

All with the harness's evidence added (below); `natural` is the suite's own pacing, about 150-400 ms of host work
between frames with the GPU idle and clocked down (P8, 285 MHz).

| Run | Result |
|---|---|
| `time_3440` alone, natural (the previous worker, before the evidence below): 10 idle, 23 under `stress-ng --cpu 8` plus a CUDA kernel loop, load average about 10 | 0 of 33 runs with a cut |
| Whole sequence order, natural, no load of ours (load average 1.6-3.8 from other work on the machine): 12 runs, 3,108 frames past each sequence's second | 1 run of 12 with wrong values, three frames within four seconds: `pan_+4x_inflight` t=11 0.363 and t=13 0.446, `cut_inflight` t=15 0.852 (over the threshold: history bit set, field zeroed for t=15..17; the flag was due at t=18, after the sequence's last frame, so the old judge passed the run) |
| The same under our load: `stress-ng` 20 CPU, 3 x 1 GiB VM, I/O, disk, fork and context-switch stressors plus the CUDA loop at 90% GPU, load average 35-42 | 0 of 3 complete runs (780 frames) |
| Whole Wine tier on the lead's side, the report this started from | 1 failing run of 6 runs of the suite |
| Frames back to back, `pan_+4x_inflight` 3,000 times | 0 of 42,000 frames differ from the first round |
| Frames really in flight (`--fast`), two sequences 2,500 times | 0 of 80,000 differ; 64,995 were still on the GPU when their slot came round |
| Paced 100-150 ms, with CUDA bursts cycling the GPU through P8/P5/P2/P0 19 times; with a compile-and-abort loop (206 in 60 s); with a second Vulkan client; with the repository's `nr-vk-passes` host suite looping (ASan build, aborting mutants) | 0 of about 1,000, 600, 1,920 and 1,920 frames |
| `time_1080p,time_3440,pan_+4x_inflight,cut_inflight` 40 times, `--fast`, shipped pass: 10 runs | 1 run of 10 changed scheduling: 3,695 of its 3,840 frames differ from the first round, sums 3.21 then 5.25; at the changes 0.364 and 0.449 (`time_3440` t=30, 31, first round) and 0.795 (`time_3440` t=16, second round, history bit set). The other 9: no change, every frame's sum 3.12-4.81 |
| The same, fixed pass: 11 runs | 0 of 42,240 frames differ; every sum 1.0000 |

So: two natural whole-order runs in 18 had wrong values (ours and the lead's), none of 36 with `time_3440` alone or
under our own CPU, memory, I/O and CUDA load. Load of that kind does not raise the rate; within these counts it
cannot be said to lower it either. What makes the GPU change how it runs the wave was not found. Both natural
incidents fell while other work was using the machine (the journal shows another worker's host suites at both
times, the compositor rendering at the third), and none of the candidates reproduced it on demand: CPU and memory
load, a compute client, GPU clock changes, a second Vulkan client, process churn. The defect does not depend on
that answer: the sums are wrong on every frame, and the fixed pass has no step whose result depends on the order.

## The deterministic case

`partial` (`tests/synth-motion-d3d12/sequences.h`): the `cut` sequence's change over one 64 px tile in four, to
content at gain 0.6 instead of 0.35. FidelityFX's formula gives 0.398 at the change, under its 0.45: not a scene
change. The shipped pass computed 0.518 and raised `SceneCut()`; the fixed pass computes 0.398319, the formula's
value, and raises nothing. With the shipped SPIR-V put back the suite fails on every sequence
(`pan_+1x: the GPU's scene-change value is not FidelityFX's formula on 13 of 13 frames: t=1: 0.000102 for 0.000006`,
..., `partial: SceneCut() raised at [9] with no cut in the content`); with the fix the largest difference over
294 frames is 1.0e-06, one unit of the pass's own fixed-point sum.

## The fix

`retoque/shaders/synth_motion/precompile/ffx/opticalflow/ffx_opticalflow_compute_scd_divergence.h`: a group barrier
after each of the ten steps that had none (five in each sum), marked `// Retoque`; the file's header says why.
Nothing else in the pass changed, and no host code. The DXIL and the SPIR-V of that one pass were rebuilt with the
code-adjacent recipe (`precompile/build.sh`); the other passes' bytecode came out byte for byte as it was.

| | before | after |
|---|---|---|
| `SynthMotion_ScdDivergence.cso` SHA-256 | `9a7af6a9...598a22` | `479372b2...9b052c` |
| `SynthMotion_ScdDivergence_Shader_Vk.spv` SHA-256 | `1dca655b...c20d5e` | `d129326b...a3cdc` |

What changes for a game:

- A scene cut resets the model's history (`NrResetWithMotion`, the D3D12 guide's `ResetIsSceneCut`) and FidelityFX's
  search writes zero vectors for four frames. A false one did both on unchanged content, whenever the GPU's
  scheduling changed; how often that is in a game was not measured. That stops.
- The value is now FidelityFX's, so the detector has the sensitivity its threshold was chosen for. The shipped sums
  read higher than the formula on most content (0.518 for 0.398 above; two to seven times on small values), so a
  change just under the threshold that used to reset the history no longer does. A real cut is unchanged: 0.99971
  for the formula's 0.99971 on the fixture's.
- Cost: ten more group synchronisations in one 27-group dispatch per frame. Not measurable in the suite's timings,
  which are unlocked and vary more than that run to run: the whole estimator at 3440x1440, median, 3.932 ms with
  the shipped pass and 3.906, 3.911 and 3.977 ms with the fixed one; at 1920x1080, 1.585 against 1.590, 1.574 and
  1.633 ms.

Two things in the same pass were left as FidelityFX has them. The groups at shift 0 and 2 read the histogram and
the previous histogram that the shift-1 groups of the same stratum overwrite at their end; the result is the
smallest of the three shifts' sums and the shift-1 sum never depends on that order, so the race cannot raise the
value, and it was not seen to change it (no round differs in 42,240 frames, and the value is the formula's on every
judged frame). The pass's atomics are relaxed; the counter that elects the last group still makes one group read
the three sums after every other has added to them.

## D3D12

The D3D12 estimator compiles the same source, so it has the same pass, and the same exposure was measured:
`tests/synth-motion-d3d12` with the DXIL as it shipped (vkd3d-proton on the same driver) fails on
`partial: SceneCut() raised at [11] with no cut in the content (FidelityFX's formula peaks at 0.398, threshold 0.45)`
and on nothing else; with the fixed DXIL it passes. That estimator exposes neither the GPU's value nor its history
bits, so its suite cannot hold the value to the formula frame by frame as the Vulkan one does: it shows the wrong
decision, not the size of the error, and a change of scheduling during its run would still show only as a stray
`SceneCut()`. Its sequences wait for every frame, so nothing of its fixture depends on frames in flight.

One difference in how the bit reaches the host, not changed here: `Estimator_Dx12` reads the read-back slot written
three confirmed frames earlier without knowing that frame completed ("old enough to have landed"), where the Vulkan
estimator reads the slot of a frame whose fence was seen. A queue more than three frames behind would hand it an
older frame's history word. That is a late or repeated report of a real bit, not a false one, and it was not
observed.

## Evidence added to the fixtures

- `tests/synth-motion-vk/harness.cpp`, per frame in the report: its ticket, the last frame seen complete when it
  was recorded, a hash of the colour uploaded and of the colour copied back from the GPU after the estimator read it
  (a mismatch fails the run), the GPU's scene-change value, history bits and group counter copied out behind the
  frame's own fence, the threshold, FidelityFX's value for the frame computed on the host, and the history bits in the
  estimator's eight read-back slots at the moment `Completed` read them.
- `tests/synth-motion-d3d12/sequences.h`: `SceneChangeReference`, FidelityFX's formula on the host (8-bit luma,
  nine strata, the filter, the three shifts, the fixed-point sums), shared by both harnesses; `partial`.
- `tests/synth-motion-d3d12/run.py`, `judge_scene_change`, for both suites: `SceneCut()` exactly where the formula
  crosses the threshold past the warm-up; no frame of the fixture within 0.03 of the threshold; and where the report
  has the GPU's value (Vulkan), that value within 2e-5 or 1% of the formula's and the history bit equal to the
  crossing.
- `tests/synth-motion-vk/run.py`: a second run of three sequences 25 times with frames really in flight; every round
  must reproduce the first bit for bit, and the first must be the formula's.

## Reproducing

The suite: `python3 tests/synth-motion-vk/run.py`. The harness's own options are in its README ("Investigating").
The investigation build that reads the estimator's histograms is not in the tree as a binary: copy
`SynthMotion_Vk.cpp`, add `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` to `CreateTexture`'s usage, and compile the harness
against the copy with `/DSYNTH_SCD_STATE`; it reports each stored histogram's sum. With the previous
`SynthMotion_ScdDivergence_Shader_Vk.h` (`git show f515c36d:retoque/shaders/synth_motion/precompile/...`) in the
include path it shows the sums above.

```sh
# from tests/synth-motion-vk/artifacts/run, with the suite's runtime environment (run.py, runtime_env)
wine synth-motion-vk-harness.exe --fast --only time_1080p,time_3440,pan_+4x_inflight,cut_inflight --repeat 40
wine synth-motion-vk-harness.exe --fast --pace 100 --only pan_+4x_inflight,cut_inflight --repeat 12
wine synth-motion-vk-harness.exe --only pan_+4x_inflight,cut_inflight,pan_+4-4 --repeat 170   # natural pacing
```

The logs, reports and helper scripts of every run above are in the ignored `x64/logs/synth-cut/` of the worktree
the work was done in; they are not part of the repository.

## Not established

- What changes the GPU's scheduling of the wave, and so the rate in a game.
- Anything on native Windows, on AMD or Intel hardware, or on another NVIDIA driver. The fix does not depend on the
  hardware (it removes an assumption), but the defect was only observed here.
- Image quality: whether the false resets were visible in a game, and whether the lower sensitivity misses cuts
  that matter. The fixture's cut and FidelityFX's threshold are the only references used.
