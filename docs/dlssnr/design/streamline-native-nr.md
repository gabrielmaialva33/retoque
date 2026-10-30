# Standing down for the game's own Streamline NR

2026-10-30. Built and host-tested (`tests/nr-sl-native-nr`); not yet run in a game that ships its own NR. Code:
`retoque/dlssnr/DlssNr_SlNativeNr.h`.

## Why

Streamline 2.14 added DLSS 3D-Guided Neural Rendering as a plugin of its own, `sl.dlss_nr` (the 2.14.0 entry of
Streamline's `changelog.txt`). Titles are reported to ship it (NBA 2K27, Portal RTX, Hogwarts Legacy from
2026-11-05; reported to us, not verified here). A game that runs NR through Streamline while Retoque's NR is on would
have the picture given NR twice: once by the game, once by this pass, on top of each other. Retoque's NR stands down
for that, for as long as the game's runs. The game's NR is never touched: no call is changed, refused or reordered.

## The constants

Read from NVIDIA-RTX/Streamline, tag `v2.14.1` (commit `2122257e0f`, released 2026-09-08), `include/sl_core_types.h`;
unchanged at `v2.14.16` (2026-10-16). Absent from `v2.12.0` and from the vendored `external/streamline`, which is
2.11.1 and is not edited, so they are local constants with this citation:

| Name | Value | Header comment |
|---|---|---|
| `kFeatureDLSS_NR` | 1004 | (none); `getFeatureFilenameAsStrNoSL` maps it to `dlss_nr` (`sl_helpers.h`) |
| `kBufferTypeUpliftInputColor` | 70 | input colour for neural-net "uplift" passes |
| `kBufferTypeUpliftOutputColor` | 71 | output colour; may alias the input (in-place uplift) |
| `kBufferTypeUpliftControlMask` | 72 | optional 4-channel control mask consumed by uplift passes |

The public SDK has no `sl_dlss_nr.h`, no programming guide for it and no plugin source at either tag, so how a game
drives the feature (options, modes, which call carries the frame) is not documented. 2.12 also inserted
`kBufferTypeResponsivityMask = 68`, moving `kBufferTypeUIAlpha` from 68 (vendored 2.11.1) to 69; nothing here uses
either, and that drift is left for the next Streamline header update.

## The signal

Three were reachable; one decides.

- **An `slEvaluateFeature(kFeatureDLSS_NR)` that Streamline accepted (returned `eOk`).** Decides. The game asked for
  NR on this frame and Streamline took it. `hkslEvaluateFeature` is installed with every Streamline 2 interposer,
  whatever the frame-generation input, so it is seen in every Streamline game OptiScaler hooks. A refused evaluate
  (plugin missing, feature unsupported) is counted and logged, and decides nothing: no NR ran.
- **`kFeatureDLSS_NR` in slInit's `featuresToLoad`.** Recorded and logged once. It means the game can run NR, not
  that it does: a game whose NR option is off still loads the plugin, and standing down then would leave no NR at
  all.
- **The uplift tags (70-72).** Recorded and logged once. `hkslSetTag`/`hkslSetTagForFrame` are installed only for a
  Streamline frame-generation input, and widening them would apply their frame-generation quirks to every game, so
  tags are read there when they are installed and from the NR evaluate's own inputs. A tag does not prove the
  feature ran. The control mask's presence is recorded for later use (log only).

The hooks only note: atomics, no lock, log, allocation or config read. NR off is unchanged.

## The stand-down

Every route asks the detector before it runs the model, with NR on:

| Route | Where | What it does |
|---|---|---|
| D3D12 after the upscaler (and the D3D11/Vulkan-on-D3D12 bridges) | `EvaluateAfterUpscale`, after the multi-view stand-down | counted as a render with the reason, nothing recorded, history reset owed |
| D3D12 before the upscaler (Stage 1) | `ScopedPreUpscale`, before the scratch loan | stood down, not declined (the after-upscale pass does not take the frame) |
| D3D12 at present (wrapped swapchain, FG hook, the game's Streamline Present) | `RunPresentPass`, after the warm-up, before any copy | the frame goes out as drawn; the route stays live |
| any D3D12 caller (the D3D11 bridge's present host included) | `DlssNr_Dx12::Dispatch`, beside `PausedWithoutFrameGen` | the backstop: before any recording, state change or tracking |
| Vulkan after the upscaler | `EvaluateAfterUpscaleVk`, before the model is claimed | its hold lapses as on any other stand-down; history reset owed |
| Vulkan present host | `DecideNr`, after the enable check, before the claim | refused as `NrSkip::GameNr` (counted in the host's NR line); the model is neither claimed nor freed |

It latches on the first ask after an accepted evaluate and releases once no accepted evaluate has arrived for 5 s
(`kQuietMs`), so a game whose NR is turned off in its own menu gets this pass back, and a short loading screen does
not flip it. Each latch, release and observation is logged once; the stand-down count every 3600.

On the after-upscale routes the game's NR evaluate usually follows the upscaler in the frame, so the first frame of
the game's NR may still carry this pass; from the next it stands down. The present routes see it the same frame.

The panel shows one amber line under the status line while NR is on and the stand-down holds:
"The game runs its own NR (Streamline DLSS-NR), so this pass stands down rather than apply NR twice. It runs again
5 s after the game's NR stops." It is a statement, never a control (DEVELOPMENT.md, 2.6).

## Not done, and why

- No setting to override it. Two NR passes on one picture is never the intended result; a user who prefers Retoque's
  turns the game's NR off in the game, and this pass returns 5 s later.
- No new hook. The decisive call is already hooked everywhere it matters.
- The model is not freed on a stand-down: freeing is a lifetime change, and the existing paths (NR off, a route that
  stops asking) already govern it.

## What a game must confirm

Nothing here has met a game with its own NR. In NBA 2K27, Portal RTX or Hogwarts Legacy (with NR on in the game and
in Retoque):

1. The log has "the game asked Streamline to load its own Neural Rendering" and, once NR renders, the latch line;
   the panel shows the line.
2. That the game drives NR through `slEvaluateFeature(1004)` at all (an `eOk` that rendered NR), and not only a
   present-time hook of the plugin's own, as DLSS-G does. If it does not, the latch never comes, and the design
   must move to the next signal (the uplift tags, which need `slSetTag` hooked whatever the FG input).
3. That an accepted evaluate means NR rendered, and that a game with its NR option off does not evaluate it (or
   gets a refusal).
4. The picture: one NR, not two, with Retoque's NR on; and Retoque's NR back 5 s after the game's is turned off.
5. Portal RTX runs on RTX Remix (Vulkan, `dxvk-remix`). Its public source drives DLSS-NR through NGX directly
   (`src/dxvk/rtx_render/rtx_ngx_wrapper.cpp`) and has no `slEvaluateFeature` call (GitHub code search, 2026-10-30),
   so its NR would not be seen by this detector at all. Whether its shipped build goes through `sl.dlss_nr` must be
   checked before it can be a validation title; if it does not, an NGX-level signal (the game creating feature 18
   itself) is separate work.
