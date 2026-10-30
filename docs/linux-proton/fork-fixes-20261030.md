# FG swapchain lock and FG object lifetime fixes (2026-10-30)

Fixes from other GPL-3.0 forks, verified against this tree and adapted rather than merged. No NR model, shader,
config key, default or deployment change. Host-tested only; see the validation boundaries below.

## Alt-tab/resize re-entering present

Source: ShyVortex's [hhkbble/OptiScaler 5181a07a](https://github.com/hhkbble/OptiScaler/commit/5181a07a).

`WrappedIDXGISwapChain4::Present` holds the swapchain's non-recursive `_localMutex` as owner 4 and `Present1` as
owner 5. DXGI can deliver alt-tab/resize window messages inside present, and a game that resizes from its window
procedure re-enters `SetFullscreenState`, `ResizeBuffers` or `ResizeBuffers1` on the same thread. Those skipped the
wait only for owner 4 under the nvngx DLSS-G replacement; any other combination (every `Present1` game, and `Present`
without the replacement) waited for itself forever. OptiScaler's own FG present holds the FG mutex as owner 2 for the
whole present, and the same methods then wanted it as owner 3: a second self-wait when `FGUseMutexForSwapchain` is
on (the default).

The source skips whenever owner 4 or 5 holds the lock under DLSS-G, and its follow-up (2faca8c0) whenever any present
holds it. Both drop the wait for other threads too. Retoque instead records the holding thread in `OwnedMutex`
(`isHeldByCurrentThread`, set after acquiring, cleared before releasing) and skips only on that thread, for both the
local and the FG mutex. Upstream's owner-4 nvngx rule is kept unchanged. Other threads still wait. Lock order is
unchanged: no new acquisition, only waits removed on the holder's thread.

Not covered: a provider whose present runs on its own thread and then waits for a window-procedure call on the game
thread. Thread identity cannot see that cross-thread cycle; upstream's owner-4 rule remains the only mitigation.

## Shared D3D12 upscaler menu

Source: [janblade 07f52a9e](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/07f52a9e).

With the legacy menu (`OverlayMenu=false`), every D3D12 feature shares one static `Menu_Dx12`. A replaced or failed
feature is destroyed two seconds later on its own thread, and its destructor resets the menu. In RE2 an XeSS to DLSS
switch dropped XeSS and the DLSS feature that failed to initialise; both resets ran at once, raced the drawing
render thread, and the game froze and crashed. `IFeature_Dx12::ImguiMutex` (recursive, as in the source) now covers
creation in `Init`, the draw in `RenderLegacyMenu` (direct and deferred) and the destructor's reset.

Lock order: taken last. Under it only MenuCommon's leaf mutexes are taken; no Retoque lock holder may create, draw or
destroy a feature's menu. Unchanged and still open: the reset frees the menu's descriptor heaps and render targets
immediately, although the running feature's last draw may still be in flight. That pre-existing GPU lifetime
needs its own design; this change does not extend or shorten it.

## FG object lifetime (HelixSR-Scaler)

Sources: lonewolf0622/HelixSR-Scaler, GPL-3.0. [5e1b0492](https://github.com/lonewolf0622/HelixSR-Scaler/commit/5e1b0492d050ded74cac4780555aefc9eb92e65f)
and [b8f18498](https://github.com/lonewolf0622/HelixSR-Scaler/commit/b8f184987cc5f16fc283919798ae9dd6c91e5f64)
were already merged.

| Commit | In this tree | Change |
|---|---|---|
| [01b95a7d97](https://github.com/lonewolf0622/HelixSR-Scaler/commit/01b95a7d9773ce49c94a39c2a14901afecd370ad) | Present: readiness is slot 0's allocator only | DLSSG/XeFG `CreateObjects` release a partial, never-submitted set; the getters refuse a missing slot |
| [1c90e142e8](https://github.com/lonewolf0622/HelixSR-Scaler/commit/1c90e142e830383e461e2a494c67cdaafbd95551) | Present in DLSSG and XeFG | The new HUD-less format is remembered: one restart, not one per frame |
| [49a25a3d9a](https://github.com/lonewolf0622/HelixSR-Scaler/commit/49a25a3d9afd0e47143fc99e07f746d082efd80b) | Present in DLSSG | The COPY_DEST->COPY_SOURCE restore is removed: the input never left COPY_SOURCE and the list could be null |
| [fba8eb77d7](https://github.com/lonewolf0622/HelixSR-Scaler/commit/fba8eb77d72d08498f72a7540b7db541855312b2) | Not applicable | FG objects are never deleted here (`currentFG` is set once); fences/events are created once and reused across restarts. Not ported; a guard fails if that ownership changes |

The fence port was declined because its destructor would never run here. A port is required if `currentFG`
ever becomes deletable. Then release the UI fence only after its last value has been proven complete; otherwise
leak it deliberately and log it.

Found, not changed: XeFG's restore barrier has the same shape. It records COPY_DEST->COPY_SOURCE whenever the
input arrived in COPY_SOURCE, but the forward move happens only for XeFG runtimes older than 1.2.2. Newer runtimes
get a barrier with the wrong before-state, possibly on a null list. It needs the debug layer before a change.

## Validation boundaries

Host suites, real code against fakes under ASan/UBSan: `fg-present-reentry`, `upscaler-menu-lock`,
`fg-command-objects`, plus the existing `fg-locks` (its fakes now include `<thread>` for `OwnedMutex`). No MSVC build,
Wine, GPU or game was run for this change.

Before integration, on native Windows with the D3D12 debug layer and in Proton:

- alt-tab, window resize and exclusive fullscreen toggles under DLSS-G (game's and nvngx replacement), FSR-FG and XeFG
  with `Present` and `Present1` games;
- an RE Engine title with the legacy menu, switching upscalers including a failing DLSS init;
- DLSS-G and XeFG with a HUD-less input that changes format (e.g. an HDR toggle), checking for one FG restart;
- DLSS-G with a COPY_SOURCE input, valid now and valid until present, with no barrier validation errors;
- forced `CreateCommandList` failure, if a fault-injection layer is available.
