# DX menu window ownership and NR Status

2026-11-26. Implemented on the menu port branch; host evidence only. MSVC/DLL build, GPU/game,
Proton and native Windows checks are pending.

## Window ownership bug fix

The DX menu and input have a single context. Following each Present HWND makes two active windows
reinitialize it continuously. The first non-null presenting window now owns it. Presents from another
window return before device capture, queue mutation, backend cleanup, Init and drawing while the owner
is alive and presented less than 1000 ms ago. Only accepted presents refresh its lease. A destroyed owner
or an age of at least 1000 ms permits handover; this retains launcher/splash-to-game transitions.
The existing recursive overlay mutex serializes the decision and renderer work. No additional lock order,
GPU resource lifetime or Vulkan changes. OverlayMenu=false retains its old path. This fixes existing menu
behavior regardless of NR enablement, as required by the port brief.

Adapted from ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass `5e3a5a3d` (GPL-3.0).
Secondary-window Resize/Release can still trigger the existing cleanup; restricting those entrypoints or
selecting the largest/foreground window is outside this change. If the owner pauses for a second, a secondary
window can take over intentionally. A reused HWND without an intervening distinct present cannot be identified
by the HWND-only policy; no new window-generation tracking is claimed.

## Input destruction bug fix

Retoque already forwards by HWND and refuses re-subclassing over a foreign WndProc chain. Retain that map and
policy instead of importing the source fork's different lost-chain container. On WM_NCDESTROY snapshot that
window's original under the existing input mutex, forward exactly once outside the lock, then erase its entry
under the mutex. This occurs before ordinary input handling and works for lost chains on non-current windows.
An unknown window goes to DefWindowProc. No other window entry is removed. Inspired by `66d9ff2d` in the same
GPL-3.0 source. This prevents the destroyed window's original from being reused for a later HWND incarnation.

## Read-only NR Status

The upstream page only calls `DlssNr::RenderStatus`; collection/drawing lives in `DlssNr_Menu.cpp`, and the
wording lives in `retoque/menu/NrStatusRules.h`. Inspired by janblade/OptiScaler-F5-DLSSNR-Multipass
`8c7d6c72` / `0b29d5c5` (GPL-3.0), reimplemented without that fork's F5Low/weights features.

| State | Rows |
|---|---|
| NR off, even if a model remains loaded | Only Off; do not query NR getters |
| Enabled but driver refuses / no model and failure reason | Failed / unavailable and the reason |
| Enabled, neither backend reports a model nor failure | On, waiting for the model |
| D3D12 / native Vulkan model retained | On with backend, explicitly model loaded; edit hidden when Apply is off |
| Enabled, all states | Hook status (hidden for the native D3D11 no-pass notice); D3D11 route notice if reported; requested passes; resolved regular and RR scale settings; startup model selection; requested cadence |
| D3D12 model retained | Also the existing cadence status, when supplied |

Uses the same `IsRunning`, `IsRunningVk`, `FailureReason`, `HookStatus`, D3D11 route and Config sources as
[Copy NR diagnostics](diagnostic-export.md). Driver refusal is also shown before a model is initialized.
Model-loaded is deliberately not a claim that a frame just executed: those getters retain the model. Requested
passes/cadence and resolved settings are not the last confirmed GPU sample. Both regular and RR scales are named,
so the summary does not guess which route actually consumed them. The model name uses the frozen startup key,
auto is a search selection rather than a loaded-file claim, and folders are stripped for screenshots.
No file search, NGX Describe, timing contract assembly, retry, state write, control or new config key. Text follows
the upstream English Status page; translated NR panel behavior is unchanged. Values wrap inside the table cell.

## Evidence and acceptance boundary

`tests/menu-status-owner` executes pure rules, the actual WndProc body with Win32 fakes and the Status collector/drawer
with Config/NR/ImGui fakes, plus source wiring guards.
The pre-fix regression fails because WM_NCDESTROY is missing. The post-fix matrix includes two alternating windows,
owner destruction, handover at exactly 1 s, lost chain forwarding and erasure, reused HWNDs and all Status rows above.
Existing NR panel/export tests remain required. Host fixtures cannot prove visual layout, actual OS callback order,
foreign overlay behavior, game stability or GPU completion. A follow-up must build the DLL, run a two-window game
with another subclassing overlay, destroy/recreate a pop-out, exercise launcher handover, and inspect Status with
NR off, failed, D3D12 and Vulkan. No game/DLL deployment is part of this port.
