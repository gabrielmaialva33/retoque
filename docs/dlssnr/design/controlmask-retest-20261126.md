# ControlMask re-test: parameter overload and channel response

2026-11-26. Source `6e410e2d5fc4b8efcc12989ad215315ff2eae0ca` plus the uncommitted,
test-only full matrix. RTX 4090 (Ada), driver **620.73.04**, preset **0**.

**The tested parameter overload explains the fixture's previous absence of mask effect.**
Across five evaluable model files, all `unsigned long long` mask arms were byte-identical to
their unmasked controls; `ID3D12Resource*` mask arms changed the image. This includes the
310.8.2 hash used by the September measurement. A runtime-version change is therefore not
required to observe ControlMask response. No production NR implementation or control was changed.

The official 310.9.16 file refused `CreateFeature(18)` with **0xBAD00001** on this Ada GPU,
after successful snippet initialization, with **0 attempts / 0 successes / 0 presents**.
It has no mask-effect result. The compat-FP16 310.9.16 file completed the experiment.

## Coverage and findings

Each measured file completed **184 trials, 5888 successful evaluations, 184 ApplyModel=0
controls, 1288 capture pairs and 380 analyzed comparison pairs**. Five measured files total
920 trials / 29440 successful evaluations / 6440 capture pairs. The official refusal adds no
evaluations. Four runners exited normally; 310.8.2 completed after its launcher timed out and
was analyzed separately, as detailed below. These outcomes are not six ordinary PASS runs.

The [fixture recipe](../../../tests/dlssnr-loopback/README.md) defines the full matrix:
R8 and RGBA=(R,0,0,1), (R,1,1,1), (R,R,R,1), requested R=0/0.25/0.5/0.75/1
(bytes 0/64/128/191/255), blend modes 0–3 and both setters. Both resource overloads are
cleared before the selected final write. The remaining pipeline and settings are shared:
1280×720 SDR, one pass, zero guides, full subrect, uniform masks, AutoMask=1, skin=-1,
intensity/tone/structure=1, UICorrection=1, a fresh feature per trial and 32 frames.
Frames 1/8/15/16/24/31 are measured; frame 0 is the ApplyModel=0 control. HUD/rest contain
262216 / 659384 pixels. Comparisons use eight-bit RGB readbacks.

All five analyses passed source-identity, exact ApplyModel=0, positive unmasked effect in both
regions, repeated/closing null and null-between-setters controls. In every measured model:

- All 80 u64 masked trials equaled the matching null output at all six measured frames.
- Typed R=0 reproduced the input exactly in all four channel arms and blend modes.
- R8 and RGBA=(R,0,0,1) matched exactly across every value, blend and measured frame.
  Setting G/B to one, or equal to R, changed the typed response. This supports the missing-G/B
  explanation for the R8/RGBA difference; it does not independently identify G versus B semantics.
- The four 310.8 files produced identical capture hashes for every corresponding case, and
  their typed outputs did not distinguish blend modes 0–3 in this fixture.
- Compat 310.9.16 distinguished the blend modes. In mode 3 at frame 31, typed R8's rest-region
  MAE was 0 / 0.4120 / 0.9580 / 1.5316 / 2.0677 versus 3.2454 unmasked. RGBA=(R,1,1,1)
  instead reached 3.2454 at R=1. Equal rounded MAE does not imply identical pixels: the latter
  ones-vs-null comparison differs by up to one channel level over the six frames in modes 1–3.

The older no-effect observation remains evidence for its original setup. The new paired experiment
shows that successful u64 round-trip does not imply the model consumes that resource through the
same overload. It does not establish the model's internal parameter-storage implementation.

## Measured response

| Model / hash prefix | Controls | Max difference: u64 mask vs null | Max difference: R8 vs R001 | Max difference: typed R=0 vs input | Typed outputs equal across blend modes |
|---|---|---:|---:|---:|---|
| 310.8.0.0-4b8d19bc3eff | PASS | 0 | 0 | 0 | True |
| 310.8.1.0-4c5bd1171c73 | PASS | 0 | 0 | 0 | True |
| 310.8.2.0-6eb209e764f3 | PASS | 0 | 0 | 0 | True |
| 310.8.3.0-f95feb54137e | PASS | 0 | 0 | 0 | True |
| 310.9.16.0-official-2bc19bc09a7b | Not measured | — | — | — | — |
| 310.9.16.0-compat-cdfa6c1ec784 | PASS | 0 | 0 | 0 | False |

Maximum absolute RGB-channel differences above cover all six measured frames and both HUD/rest regions, in levels of 255. The u64 column covers all 80 masked arms per model. R8/R001 covers both setters, all modes and five values. R=0 covers all 16 typed arms whose R value is zero (G/B remain one in r111). Mode equality compares SHA-256 of every typed masked capture across modes.

### Typed-mask response at frame 31

Cells below are **HUD / rest MAE versus input**, in levels of 255. Columns are requested mask values, quantized to bytes 0/64/128/191/255. `null` is the corresponding unmasked control. Modes collapse to `0–3` only when all six captured frames are byte-identical across them. Full per-frame measurements and hashes remain in each `mask-report.json`.

| Model | Blend | Arm | 0 | 0.25 | 0.5 | 0.75 | 1 |
|---|---|---|---:|---:|---:|---:|---:|
| 310.8.0.0-4b8d19bc3eff | 0–3 | null | 2.4127 / 4.2114 | — | — | — | — |
| 310.8.0.0-4b8d19bc3eff | 0–3 | r8 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.0.0-4b8d19bc3eff | 0–3 | r001 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.0.0-4b8d19bc3eff | 0–3 | r111 | 0.0000 / 0.0000 | 0.6496 / 1.0361 | 1.3890 / 2.1774 | 2.0541 / 3.2425 | 2.7220 / 4.3580 |
| 310.8.0.0-4b8d19bc3eff | 0–3 | rgb | 0.0000 / 0.0000 | 0.0697 / 0.1930 | 0.7060 / 1.0649 | 1.6007 / 2.4616 | 2.7220 / 4.3580 |
| 310.8.1.0-4c5bd1171c73 | 0–3 | null | 2.4127 / 4.2114 | — | — | — | — |
| 310.8.1.0-4c5bd1171c73 | 0–3 | r8 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.1.0-4c5bd1171c73 | 0–3 | r001 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.1.0-4c5bd1171c73 | 0–3 | r111 | 0.0000 / 0.0000 | 0.6496 / 1.0361 | 1.3890 / 2.1774 | 2.0541 / 3.2425 | 2.7220 / 4.3580 |
| 310.8.1.0-4c5bd1171c73 | 0–3 | rgb | 0.0000 / 0.0000 | 0.0697 / 0.1930 | 0.7060 / 1.0649 | 1.6007 / 2.4616 | 2.7220 / 4.3580 |
| 310.8.2.0-6eb209e764f3 | 0–3 | null | 2.4127 / 4.2114 | — | — | — | — |
| 310.8.2.0-6eb209e764f3 | 0–3 | r8 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.2.0-6eb209e764f3 | 0–3 | r001 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.2.0-6eb209e764f3 | 0–3 | r111 | 0.0000 / 0.0000 | 0.6496 / 1.0361 | 1.3890 / 2.1774 | 2.0541 / 3.2425 | 2.7220 / 4.3580 |
| 310.8.2.0-6eb209e764f3 | 0–3 | rgb | 0.0000 / 0.0000 | 0.0697 / 0.1930 | 0.7060 / 1.0649 | 1.6007 / 2.4616 | 2.7220 / 4.3580 |
| 310.8.3.0-f95feb54137e | 0–3 | null | 2.4127 / 4.2114 | — | — | — | — |
| 310.8.3.0-f95feb54137e | 0–3 | r8 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.3.0-f95feb54137e | 0–3 | r001 | 0.0000 / 0.0000 | 0.0034 / 0.0010 | 0.0129 / 0.0245 | 0.0340 / 0.0668 | 0.0904 / 0.1579 |
| 310.8.3.0-f95feb54137e | 0–3 | r111 | 0.0000 / 0.0000 | 0.6496 / 1.0361 | 1.3890 / 2.1774 | 2.0541 / 3.2425 | 2.7220 / 4.3580 |
| 310.8.3.0-f95feb54137e | 0–3 | rgb | 0.0000 / 0.0000 | 0.0697 / 0.1930 | 0.7060 / 1.0649 | 1.6007 / 2.4616 | 2.7220 / 4.3580 |
| 310.9.16.0-compat-cdfa6c1ec784 | 0 | null | 1.7843 / 3.2454 | — | — | — | — |
| 310.9.16.0-compat-cdfa6c1ec784 | 0 | r8 | 0.0000 / 0.0000 | 0.0165 / 0.0146 | 0.0407 / 0.0667 | 0.0749 / 0.1211 | 0.1252 / 0.1803 |
| 310.9.16.0-compat-cdfa6c1ec784 | 0 | r001 | 0.0000 / 0.0000 | 0.0165 / 0.0146 | 0.0407 / 0.0667 | 0.0749 / 0.1211 | 0.1252 / 0.1803 |
| 310.9.16.0-compat-cdfa6c1ec784 | 0 | r111 | 0.0000 / 0.0000 | 0.3946 / 0.7597 | 0.9522 / 1.6153 | 1.4229 / 2.4567 | 1.9195 / 3.3071 |
| 310.9.16.0-compat-cdfa6c1ec784 | 0 | rgb | 0.0000 / 0.0000 | 0.0530 / 0.1465 | 0.4581 / 0.8161 | 1.1096 / 1.8877 | 1.9195 / 3.3071 |
| 310.9.16.0-compat-cdfa6c1ec784 | 1 | null | 1.7843 / 3.2454 | — | — | — | — |
| 310.9.16.0-compat-cdfa6c1ec784 | 1 | r8 | 0.0000 / 0.0000 | 0.1068 / 0.2004 | 0.2963 / 0.4711 | 0.5670 / 0.7588 | 0.8040 / 1.0625 |
| 310.9.16.0-compat-cdfa6c1ec784 | 1 | r001 | 0.0000 / 0.0000 | 0.1068 / 0.2004 | 0.2963 / 0.4711 | 0.5670 / 0.7588 | 0.8040 / 1.0625 |
| 310.9.16.0-compat-cdfa6c1ec784 | 1 | r111 | 0.0000 / 0.0000 | 0.3584 / 0.7327 | 0.8922 / 1.5843 | 1.3284 / 2.4154 | 1.7843 / 3.2454 |
| 310.9.16.0-compat-cdfa6c1ec784 | 1 | rgb | 0.0000 / 0.0000 | 0.1525 / 0.3161 | 0.6129 / 1.0145 | 1.2187 / 2.0309 | 1.7843 / 3.2454 |
| 310.9.16.0-compat-cdfa6c1ec784 | 2 | null | 1.7843 / 3.2454 | — | — | — | — |
| 310.9.16.0-compat-cdfa6c1ec784 | 2 | r8 | 0.0000 / 0.0000 | 0.0196 / 0.0284 | 0.0480 / 0.0962 | 0.0898 / 0.1564 | 0.1453 / 0.2234 |
| 310.9.16.0-compat-cdfa6c1ec784 | 2 | r001 | 0.0000 / 0.0000 | 0.0196 / 0.0284 | 0.0480 / 0.0962 | 0.0898 / 0.1564 | 0.1453 / 0.2234 |
| 310.9.16.0-compat-cdfa6c1ec784 | 2 | r111 | 0.0000 / 0.0000 | 0.3584 / 0.7327 | 0.8922 / 1.5843 | 1.3284 / 2.4154 | 1.7843 / 3.2454 |
| 310.9.16.0-compat-cdfa6c1ec784 | 2 | rgb | 0.0000 / 0.0000 | 0.0442 / 0.1525 | 0.4302 / 0.7832 | 1.0837 / 1.8940 | 1.7843 / 3.2454 |
| 310.9.16.0-compat-cdfa6c1ec784 | 3 | null | 1.7843 / 3.2454 | — | — | — | — |
| 310.9.16.0-compat-cdfa6c1ec784 | 3 | r8 | 0.0000 / 0.0000 | 0.3157 / 0.4120 | 0.7756 / 0.9580 | 1.1982 / 1.5316 | 1.5506 / 2.0677 |
| 310.9.16.0-compat-cdfa6c1ec784 | 3 | r001 | 0.0000 / 0.0000 | 0.3157 / 0.4120 | 0.7756 / 0.9580 | 1.1982 / 1.5316 | 1.5506 / 2.0677 |
| 310.9.16.0-compat-cdfa6c1ec784 | 3 | r111 | 0.0000 / 0.0000 | 0.3584 / 0.7327 | 0.8922 / 1.5843 | 1.3284 / 2.4154 | 1.7843 / 3.2454 |
| 310.9.16.0-compat-cdfa6c1ec784 | 3 | rgb | 0.0000 / 0.0000 | 0.3039 / 0.4903 | 0.8272 / 1.2514 | 1.2974 / 2.2475 | 1.7843 / 3.2454 |

## Model identities

Versions are the embedded VS_FIXEDFILEINFO versions. SHA-256 and sizes were checked before each
run. Files remained in user-owned locations and were linked into ignored fixture staging; no
NVIDIA DLL/model was copied into tracked source or modified.

| File version | Bytes | SHA-256 | Identification |
|---|---:|---|---|
| 310.8.0.0 | 165840496 | `4b8d19bc3eff58a084f5eca7489c921501c203450169fb82ff4f649a4482ba05` | Archived runtime-test file |
| 310.8.1.0 | 165830144 | `4c5bd1171c7336b4b04fb394de51da285ab6ead6f922d7afdec163f71c319d74` | Archived runtime-test file |
| 310.8.2.0 | 165830144 | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` | Local kit; matches the September baseline |
| 310.8.3.0 | 165840496 | `f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc` | Archived model |
| 310.9.16.0 | 266424944 | `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f` | Matches the official hash recorded in [model notes](model-310-9-16.md) |
| 310.9.16.0 | 276162560 | `cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed` | Matches the compat-fp16 hash recorded in [model notes](model-310-9-16.md) |

## Build and environment identity

- Known clean Release `OptiScaler.dll`, built at merge `3cc77f18`:
  `4a75449bb56501ad370e7c2c20bdff2b0baf5ed1cc9a1f1c032e664fb9c355eb`.
- Matching `nvngx.dll_dlssnr.dll` forwarder:
  `4eff10e2d17780c4532eb4f9f6370ff4f1ffb7875f52b1d505a449c7d44079d7`.
- Newly compiled harness, reused unchanged for all six files:
  `06248026ff0889a93115d7cdfd0737fb4b4f077da662838939f975d6603dd013`.
- Driver `_nvngx.dll`:
  `637a9390855b16c7f6c6a723e5fc97ab11bca20224064780f25dcff8b774892a`.
- Proton Experimental: `1794497090 experimental-11.0-20261112-x86_64`.

The trees at 3cc77f18 and 6e410e2d are not literally identical: their only changed file is
`docs/linux-proton/upstream-20261126.md`. Code is identical. The standalone present fixture
requires the known DLL path and selects its adjacent forwarder, but **does not load OptiScaler.dll**.
It drives feature 18 through that forwarder and the compiled-in production composition bytecode;
this is not a test of the full injected OptiScaler hook path.

The harness was built once via the runner's existing MSVC prefix lease, with its build log redirected
to a file. No build-local invocation, prefix reset/kill or shader regeneration was performed.

## Commands and retained evidence

`KNOWN_BUILD` is the coordinator-supplied external build folder; `MODEL` selects the external file
with the hash from the table. The exact expanded argv, model paths, times and return codes for each
invocation are retained in `artifacts/controlmask-experiment/receipts.json` and each `receipt.json`.

```bash
# First model: build under the existing runner lease, then execute.
LOOPBACK_MODEL="$MODEL" TMPDIR="$PWD/.controlmask-tmp" python3 tests/dlssnr-loopback/run.py --present-nr --mask-matrix --dll "$KNOWN_BUILD/OptiScaler.dll" > "$RUN_LOG" 2>&1
# Subsequent models: the same executable, serially, no compiler invocation.
LOOPBACK_MODEL="$MODEL" TMPDIR="$PWD/.controlmask-tmp" python3 tests/dlssnr-loopback/run.py --present-nr --mask-matrix --skip-build --dll "$KNOWN_BUILD/OptiScaler.dll" > "$RUN_LOG" 2>&1
# Only the recovered 310.8.2 run needed a separate analyzer invocation, after full coverage.
python3 tests/dlssnr-loopback/analyze_mask.py tests/dlssnr-loopback/artifacts/mask-matrix-run --matrix > artifacts/controlmask-experiment/310.8.2.0-recovered-analysis.log 2>&1
```

The local orchestration scripts and `inventory.json`, `source-hashes.json`, `summaries.json`,
`tables.md`, runner/Proton/harness/build logs, model provenance, report hashes and all five
`mask-report.json` files remain under `artifacts/controlmask-experiment/`. Each archive is named
by version and the first 12 hash characters. GPU runs never overlapped. The next run was blocked
until the preceding process stopped mapping its files.

310.8.2 initially presented about one frame per second while its window was on an inactive
workspace. Memory and I/O pressure were absent. The legacy Hyprland IPC adjustment commands
were rejected and changed nothing. A runtime-only Lua rule scoped to class `steam_proton` and
exact title `dlssnr-loopback` enabled `render_unfocused`, leaving the global 15 fps setting,
clocks and VRR unchanged. The launcher still reached its pre-existing 600-second timeout; the
guard stopped the serial driver before any next model could launch. The harness was allowed to
finish naturally, then its complete 184/5888/1288 receipt and full analyzer were checked. No child
or prefix was killed/reset by the worker. Its runner return code remains 1; recovered analysis
returned 0. All capture hashes matched the other 310.8 runs. A recovery-script log initially
hit non-UTF-8 NGX text; the retry used the runner's existing `errors='replace' log-reading policy.
The temporary compositor rule was disabled and its Lua handle discarded after the last GPU
process exited. `window-rendering.json` records the attempts and cleanup.

Raw captures exceeded 20 GB after the third and fifth measured analyses. After saving each
analysis, the four 310.8 raw frame sets were pruned: **10304 PPM files / 28488664064 bytes**.
Their complete capture hashes, metrics, cover images and textual receipts remain; compat-FP16
raw captures remain (2576 frame files / 7122166016 bytes). `pruning.json` records each deletion.
No model, forwarder or other agent's artifacts were pruned.

## Host validation and limits

The change fast-forwarded onto 6e410e2d and reapplied without conflicts. The capped command
below passed **156/156**, with zero failures/skips/timeouts/aborts/inconclusive results:

```bash
systemd-run --user --scope -q -p MemoryHigh=6G -p MemoryMax=7G -p MemorySwapMax=1G -p OOMPolicy=kill -- env TMPDIR="$PWD/.controlmask-tmp" python3 tests/run_all.py --tier host --jobs 2 --strict --log-dir "$PWD/artifacts/rebase-host" > artifacts/rebase-host.log 2>&1
python3 tools/check_docs.py
```

The earlier pre-rebase run remains 155 PASS / 1 FAIL because its TMPDIR contained `artifacts`,
which the existing nr-replay-format traversal test rejects. The later complete green run used
a temporary path without that component. The original failure was not relabeled as PASS.
The new host suite has six tests, including compiled C++/Python matrix parity, the actual full
orchestration slice, typed setter cleanup/readback, count receipts, synthetic analysis, invalid
evidence refusal and the legacy 24-case analyzer. The old 24-case / 768-evaluation mode remains
selectable; it was not rerun on GPU in this session. CI NumPy/Pillow installation was added;
remote CI was not run.

These are static synthetic SDR results on one Ada GPU, driver and preset, with uniform masks
and eight-bit captures. They do not qualify game behavior, native Windows/D3D12 correctness,
Vulkan, other GPUs/presets, temporal motion, sub-quantization differences, image quality or
performance. The external probe uses a different FP16 pipeline, half-image masks, UI/skin defaults
and frame count; its numerical result was not reproduced exactly. There is no new production
ControlMask UI, config key or runtime behavior. A production use of the typed setter would need
its own authorized implementation and route/runtime validation.

## Initial source audit, before the GPU runs


The [September measurement](../../../tests/dlssnr-loopback/control-mask.md) reports thirteen
byte-identical masked/unmasked comparisons on 310.8.2.0. Preserve that result with its original
conditions; it does not establish that every runtime ignores ControlMask.

At baseline, the [loopback fixture](../../../tests/dlssnr-loopback/README.md) already implements the October
24-trial re-test. Its matrix in `present_nr.h` includes one R8 zero trial, RGBA8 channel isolation,
R=128 and blend modes 0–3 for RGBA8 ones/B=0. It did **not** implement R8 and RGBA8 at
0 / 0.25 / 0.5 / 0.75 / 1 under every blend mode. There were no CLI arguments for those values.
`run.py` checks exactly 24 trials/768 evaluations/168 capture pairs, and `analyze_mask.py` names
those trials explicitly. Changing only the trial table would leave the coverage oracle inconsistent.

The read-only source comparison was against `MFGAdaUnlock-RenoDx`, commit
`438f5dac2e48d9d73564cd167eb3289abee48b99`, `tests/nr_probe.cpp`:

- `--control-mask N` writes blend mode at creation and evaluation; `--mask-value V` fills the
  left half with `round(V * 255)` and the right half with zero. The texture is **R8_UNORM only**.
- The mask is allocated after feature creation and supplied at evaluation through the
  `ID3D12Resource*` setter. Its subrect covers the full image.
- Its default controls are auto mask 1, skin/local structure/intensity/tone 1, UICorrection 0,
  preset 0 and 10 static-image frames. Input/output are FP16; final RGB differences use clamped,
  rounded eight-bit output. It reports separate left/right MAE and whole-image MAE.
- Retoque instead sets the mask before create and evaluate through `unsigned long long`, reads
  it back through that same overload, uses 32 frames and its composition pipeline, and has skin
  structure -1 in the current matrix. Successful same-type round-trip does not prove the model
  consumed the resource through its own getter. The setter difference is a test variable, **not
  a confirmed cause**.
- The external commit message reports mode-3 changes of 0.18 / 0.57 / 0.89 / 1.22 of 255 at
  0.25 / 0.5 / 0.75 / 1, versus 2.34 unmasked, and untouched pixels at zero in every mode.
  Those are **external reported numbers**, not reproduced here. The message does not identify
  the tested DLL hash or command line; the assignment associates them with 310.9.16, but that
  identity cannot be independently established from this message alone.

The proposed R/G/B interpretation remains a hypothesis. R8 and RGBA8 comparisons must distinguish
RGBA=(R,0,0,1), RGBA=(R,1,1,1), and equal RGB channels before attributing attenuation to missing G/B.
Also hold auto mask, preset, setter type, feature reset and image pipeline constant. Keep null repeats,
positive unmasked effect and source-identity controls; do not interpret a failed feature creation as
an inert mask.

