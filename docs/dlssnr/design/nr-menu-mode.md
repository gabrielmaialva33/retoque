# Menu Mode: Sustaining Neural Rendering During Inactive Upscaler Windows

Status: **design note; not yet implemented.** Proposes optional continuation of Neural Rendering during game menus, inventories, and pause screens when upscaler evaluation calls cease.

Re-surveyed 2026-10-29 against `c862bd2e`: §2 is rewritten route by route (its old line citations had gone stale, and
it missed the two routes that already continue), and §3 gains the corrections in §3.3. Still no code: the change is
not small (a new verdict in the present pass, a key, the Vulkan owner's idle rule) and it moves NR onto frames with
the interface already drawn, which `retoque/dlssnr/README.md` ("HUD detection was tried and removed") says is safe
today only because the pass runs before the interface.

---

## 1. Where this comes from

In many modern engines, opening a menu or pause screen halts game world rendering and stops upscaler evaluations entirely. In current OptiScaler DLSS-NR builds, this causes an abrupt visual disconnect: Neural Rendering instantly disengages, resulting in noticeable luminance and contrast pops while browsing menus.

The solution is established across community projects:

1. **optimizer-fps-dlss5** (BeliyG3, commit `1714312`, October 2026, MIT licence):
   - `docs/MENU_MODE.md:8-10`:
     > *"It starts on its own at least 150 ms and 5 presents after the game's last NR frame, and ends at the game's next NR frame. That frame restarts NR's history"*.
   - `hosts/reshade/menu_state.h:34-35`: defines engagement window `kEntryPresents = 5; entryMs = 150.0`.
   - `hosts/reshade/menu_guides.cpp:25,103`: snapshots the last valid depth buffer and feeds a synthesized zero-motion vector field.
   - Acceptance validation (`docs/dev/menu-mode-acceptance.md`): output quality measured 44–49 dB PSNR against reference; transition reset cost measured at 5.7 dB across the first five recovery frames.
   - Verified in-game in *Star Wars Jedi: Fallen Order* via OptiScaler's D3D11 bridge.
2. **RenoDX** (`v8.5dev`, MIT licence):
   - `present_path.hpp:40-43, 161-169`: treats guide captures older than the age limit as menu/loading states, continuing presentation passes on neutral guides with bidirectional history resets.

---

## 2. What exists in this codebase today (re-surveyed 2026-10-29)

No route has a menu mode. Two present-time hosts already keep NR on frames without an upscaler call, by
construction; the default route and the Vulkan route do not. Line numbers are against `c862bd2e`.

| Route | A menu frame (the game stopped calling its upscaler) | Where |
|---|---|---|
| D3D12 after-upscale, `HookMethod=1` (the default) | **No NR.** Every present takes the `Upscaled` verdict, reports status and returns; nothing takes over when the evaluates stop. The status line can keep saying "active". | `DlssNr_PresentBoundary.h:81-82` (`Decide`), `DlssNr_Dx12.cpp:4714-4721`; `hellraiser-video-present.md:9-12` |
| `HookMethod=0` (Auto) | **No NR** once it has seen the game's DLSS-G (it behaves as 1, `PresentBoundary.h:47-53`). Before that, on a lower present entry, the same as the next row. | |
| D3D12 present, `HookMethod=2`, lower entries (wrapped swapchain, OptiScaler's own FG hook) | **No NR after the first capture**: once the newest capture has been consumed, an unchanged newest sequence skips every later present (`!boundary`), whatever `RequireDlss` says. That skip exists because below the boundary no new capture can mean a generated frame. | `DlssNr_Dx12.cpp:4951-4965` |
| D3D12 present, `HookMethod=2`, the game's own Present (Streamline boundary) | **Continues**, with `RequireDlss=false`: the frame runs on constant depth and zero motion (or `SynthMotion`), and the model's history restarts once the new source has held for 4 frames. With `RequireDlss=true` the frame is shown as drawn. The boundary probe is installed only for `HookMethod=2`, so Auto never gets here. | `DlssNr_Dx12.cpp:5050-5101`, `:3746-3762`; `DlssNr_PresentGuideIdentity.h:41` (`kSourceStable`); `DlssNr_SlBoundaryProbe.cpp:522`; `hellraiser-video-present.md` |
| D3D11 bridge (built only at `HookMethod=2`) | **Continues**: the host runs every present, with the captured guides when they are the frame's and zeros or `SynthMotion` otherwise. No skip for an unchanged capture. | `DlssNr_PresentHost.cpp:683-760`; `dx11_with_dx12_sc.cpp:59` |
| Vulkan after-upscale | **No NR**: the route simply stops. | |
| Vulkan present host (`VulkanPresentHost`, off by default) | **No NR for 10 s**, then cold NR on zero guides: the host is refused while the after-upscale route "owns the model", takes it back only after `kAfterUpscaleIdleMs` = 10 s without an evaluate (draining the device's queues, freeing the route's objects), and hands it back when DLSS returns, at the cost of two feature rebuilds. The 10 s was chosen so a menu does *not* cost those rebuilds, which makes it the opposite of menu continuation. Under vkd3d-proton it stands down for good once the D3D12 model exists. | `DlssNr_VkModelOwner.h:51-56`, `DlssNr_VkPresentHost.cpp:1615-1622`, `DlssNrFeature_Vk.cpp:4099, 4116`; `nr-vulkan-present-host.md:1109-1115` |

Two things work against continuation on purpose and must be reconciled, not overridden:

- **`PauseWithoutFrameGen`** (`pause-without-frame-gen.md`, `DlssNr_Dx12_Lifetime.cpp:98`) skips NR while the game has
  turned its own DLSS-G off, which is exactly what many menus do. With it on, menu mode would be asked to run on frames
  the user asked to leave alone; the two need a stated precedence (proposed: `PauseWithoutFrameGen` wins).
- **The interface is in the frame.** On the after-upscale route the pass runs before the game draws its UI; at present
  it does not. The UI mask (`DlssNr_UiMask_Dx12`, `hud-protection.md`) is the only protection the present route has.

And one document disagreed with the code (corrected in the same change as this survey): `present-real-guides.md:83` and `:198` said Auto "stays on the present route
after the upscaler stops, so the zero-guide frames in a menu or cutscene keep being edited". On a lower entry the skip
at `DlssNr_Dx12.cpp:4951` stops that after the first capture, and Auto never reaches the boundary, where the claim
would hold. The claim is true only of `HookMethod=2` at the boundary and of the D3D11 bridge.

Reusable pieces: `DlssNr_ZeroGuides.h/.cpp` (constant depth, zero motion), `ZeroGuideReset`, `SynthMotion`, the
source-change reset (`g_sourceIsPresent`, `DlssNr_Dx12.cpp:5110` and `:5483`), the UI mask, and the per-entry
boundary flag that already lets the boundary continue where lower entries skip.

---

## 3. Proposed design

An opt-in configuration (`[DlssNr] MenuMode=true`, default `false`) that bridges upscaler inactivity:

```
[ Normal Gameplay ] ───▶ Upscaler evaluates each frame ───▶ NR runs via normal path
                                  │
                       (Pause menu opened: >150 ms & >5 presents since last evaluate)
                                  │
                                  ▼
[ Menu Mode Active ] ──▶ Force NR model reset
                     ──▶ Engage present hook with:
                           • Swapchain backbuffer as color input
                           • Captured last-frame depth (or 1.0f neutral plane)
                           • Zero motion vectors
                           • UI Protection Mask active over 2D interface
                                  │
                       (Game unpaused: new upscaler evaluate detected)
                                  │
                                  ▼
[ Return to Gameplay ] ─▶ Force NR model reset ──▶ Resume normal upscaler path
```

### 3.1 Transition state machine
- **Engagement condition:**
  $t_{\text{current}} - t_{\text{last\_upscale}} \ge 150 \text{ ms} \quad \text{and} \quad \text{presents\_since\_upscale} \ge 5$.
- **Entry action:**
  Set `g_nr.reset = true` (flushes previous motion history so in-game motion does not smear into static menu elements).
- **Execution:**
  Route frame through `RunPresentPass`. Feed `depth` from last captured frame, `motion` from zero-cleared buffer, and run `DlssNr_UiMask` to mask out fonts, buttons, and HUD boxes.
- **Exit action:**
  Upon observing the next real upscale evaluate call, immediately trigger another `g_nr.reset = true` to prevent menu imagery from corrupting the returning 3D camera history.

### 3.2 Handling `HookMethod=1` (Upscaled mode)
Under `HookMethod=1`, `RunPresentPass` normally stands down entirely. When `MenuMode` is enabled and menu inactivity is confirmed, `RunPresentPass` temporarily activates as an opportunistic fallback, relinquishing presentation as soon as the upscaler evaluation resumes.

### 3.3 Corrections from the 2026-10-29 survey

- **Where it lives.** Not a timeout inside `RunPresentPass` alone: a new verdict (`MenuRun`) from
  `DlssNr::PresentBoundary::Decide`, with the time of the last `++g_upscaleRenders` (`DlssNr_Dx12.cpp:5479`) and the
  presents since, decided by a pure header with a host test in the shape of `DlssNr_PresentBoundary.h`. `MenuRun`
  forces zero guides (or `SynthMotion`) and bypasses the unchanged-capture skip at `:4951` for that present only: a
  generated frame never arrives 150 ms after a real one, which is what makes the bypass safe there.
- **Entry conditions** add to §3.1: at least one evaluate this session; neither the game's DLSS-G nor OptiScaler's FG
  active (or this is the FG hook's base-frame entry); `PauseWithoutFrameGen` not pausing; an SDR swapchain, or
  `HdrPresent` on.
- **History.** BeliyG3 keeps the model's history *into* the menu (`menu_param_book.cpp:182`, `Reset=0`) and resets only
  on the first game evaluate after a menu pass (`menu_state.h:13-16`), because his menu frames are the same surface.
  Here they are not: the after-upscale route edits linear HDR before tone mapping and the present pass a tone-mapped
  backbuffer, at another size and format. So a reset on entry and on exit is required here, and both already exist
  (the source-change reset). Expect a visible step in look at each switch; the "pop" moves, it does not disappear, and
  a size or format change also costs a feature rebuild on entry.
- **Exit.** Decided on the evaluate's CPU side, before its own present: on an engine with a lagging render thread that
  loses one frame rather than editing it twice.
- **Cost.** Menus run uncapped; the model at full rate there is what `PauseWithoutFrameGen` exists to avoid. Take
  BeliyG3's suspension rule too: three menu passes in a row at 100 ms or more and the menu stays without NR
  (`docs/MENU_MODE.md:58`).
- **Vulkan** is out of scope for a first version: a short-idle claim on the model would be two feature rebuilds per
  menu, the cost the 10 s rule was written to avoid.
- **Cadence** (§5): forced to 1 during menu mode, as written.

---

## 4. Where it goes

1. **`OptiScaler/Config.h` / `Config.cpp`:**
   Declare `CustomOptional<bool> DlssNrMenuMode { false };`, mapped to `[DlssNr] MenuMode`.
2. **`retoque/shaders/dlssnr/DlssNr_Dx12.cpp`:**
   - In `RunPresentPass`: implement timeout detection tracking elapsed presents and wall-clock time since `g_upscaleRenders` last incremented.
   - Switch to zero guides and set reset flags upon state transition.
3. **`retoque/dlssnr/DlssNr_Menu.cpp`:**
   Add toggle under the Compatibility or Advanced sub-panel.

---

## 5. Interactions with existing features

- **Invariant 1 (Default-Identical):**
  Disabled by default (`MenuMode=false`). Behavior remains byte-identical to current code when unset.
- **HUD Protection (`hud-protection.md`):**
  Crucial synergy. In menus, textual elements and panels dominate the screen. Without the UI mask, the neural model sharpens font edges inconsistently. The UI mask must remain enforced during menu mode.
- **Model Cadence (`model-cadence.md`):**
  Must automatically drop to cadence 1 (`runModel = true` every frame) during menu mode, as motion vector interpolation over zero motion adds no value and introduces phase jitter.

---

## 6. Risks

1. **Animated menu backgrounds:**
   If a pause menu renders an animated 3D character with moving camera or particle effects in the background, zero motion vectors will cause slight temporal ghosting across background motion.
2. **Unpause transition penalty:**
   BeliyG3 measured a ~5.7 dB PSNR drop across the first five frames following unpause due to the mandatory model history reset. This is visually preferable to smearing, but represents a transient settling period.

---

## 7. Test plan

1. **Host unit test (`tests/nr-menu-mode`):**
   Simulate presentation frames with injected evaluate gaps:
   - Verify engagement triggers at exactly $>150 \text{ ms}$ and $\ge 5$ presents;
   - Verify `reset` flag is pulsed on both entry and exit;
   - Confirm `MenuMode=false` never activates present hooks under `HookMethod=1`.
2. **In-game validation:**
   - Test in *Cyberpunk 2077* and *Crimson Desert*. Open inventory/map; confirm luminance stays stable and text remains crisp without artifacts.

---

## 8. Open questions

- Should the depth buffer in menu mode be frozen to the last valid 3D depth, or replaced with a flat far-plane ($1.0$) to avoid depth edge artifacts against menu UI?
