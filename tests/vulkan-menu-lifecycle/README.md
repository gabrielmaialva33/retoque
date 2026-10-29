# Vulkan menu swapchain lifecycle

`python3 tests/vulkan-menu-lifecycle/run.py` compiles the whole production
`OptiScaler/menu/menu_overlay_vk.cpp`, unmodified, in one translation unit with fakes
(`fake_include/`, `fakes.cpp`). It adds the menu block of `hkvkCreateWin32SurfaceKHR`, sliced verbatim
from `OptiScaler/hooks/Vulkan_Hooks.cpp`. It builds with `g++ -Wall -Wextra -Werror` under ASan and
UBSan. The Vulkan types and prototypes come from the real Khronos headers (`external/vulkan/include`),
and ImGui's types from the real `imgui.h` and `imgui_impl_vulkan.h`. Every Vulkan entry point, the ImGui
backend, `MenuOverlayBase` and `IsWindow` are fakes. They record an **ordered event log** (drains,
creates, destroys, backend init and shutdown, context init and shutdown, submits). They also record
**faults**: contract breaks that the real component turns into a crash, a leak or undefined behaviour.
Any fault fails the case.

The faults it knows:

- `ImGui_ImplVulkan_Shutdown` with no renderer backend. The real one dereferences a null `bd` in
  `ImGui_ImplVulkan_DestroyDeviceObjects`, and only a Debug build's `IM_ASSERT` stops it first.
- `MenuOverlayBase::Init` over a live context. `ImGui::CreateContext` leaks the old one.
- `MenuOverlayBase::Shutdown` with the Vulkan backend still attached. `ImGui::DestroyContext` leaks it.
- A destroy of an object that is not alive, and an `IM_FREE` of an unknown pointer.

Every case ends with the production teardown. All overlay objects and `IM_ALLOC` arrays must be released
then.

The fake menu base keeps `MenuCommon`'s storage type, a plain `HWND` and `bool`, and reads it with the
production getters' own `std::atomic_ref` expressions, which `run.py` cuts from `menu_common.h`; it also requires
`MenuCommon::Init` and `Shutdown` to store both through `std::atomic_ref` (Codex review FM-01, `8feb116f`). Its
earlier `std::atomic` fields hid the data race of `OnOtherWindow`'s unlocked read. A ThreadSanitizer build of
this unit reports nothing even with the plain reads put back, because the fake's event log takes one mutex on
every call and so orders the two threads: this suite cannot show that race, only hold the source to the fix.

`run.py` also checks two things in the source of `Vulkan_Hooks.cpp`. First, `MenuOverlayVk::CreateSwapchain` is
called once, inside the driver-success block of `hkvkCreateSwapchainKHR`, which is what the
cases' `GameCreateSwapchain` models. Second, the surface hook's block still has the shape that gets sliced.

## Cases

| Case | Asserts |
|---|---|
| second window never touches the menu | A surface and a swapchain on window B, while the menu is on A's chain and A exists, make no drain, no release, no backend or context change, and do not move the hook's fallback `_hwnd`. A is still drawn on. B's present passes through with its info byte-identical. |
| two chains on one window hand over | A recreate with `oldSwapchain` on the same window drains once, releases the whole old generation before building the new one, and keeps the ImGui context. It rebuilds the backend once. The retired chain passes through and the new one is drawn on. |
| concurrent first attach from two threads | Two real threads. Thread B is held right after its pre-lock `IsInited()` read until thread A finishes its create, which is Codex's interleaving. Exactly one owner (A), one context made and none destroyed, no drain or release by B, one generation alive. |
| free-running first attach, 200 rounds | The same two threads with no forced order, each round on new windows (the previous pair destroyed). Each round ends with one owner, one new context and one generation, and no faults. |
| failing vkCreateSwapchainKHR leaves the menu consistent | A driver failure never reaches the overlay, and the old chain keeps its menu (a retired chain can still present). An overlay-side failure (the second `vkCreateFramebuffer`) publishes nothing, releases the partial generation and its arrays, and leaves no backend. Presents pass through, and the next create recovers. |
| failed drain keeps the old generation | A failed `vkDeviceWaitIdle` before a recreate releases nothing and builds nothing, and the menu stops drawing. The next successful drain releases the old generation and recovers. |
| destroyed menu window, create re-binds | The menu's window is destroyed, then a chain is created on window B. The renderer backend shuts down exactly once, the context moves to B once, and B owns and draws the menu. This is run with and without the surface hook first. |
| surface hook that lost the race leaves the menu alone | Two real threads. The surface hook for window B is held right after its pre-lock `IsInited()` read until window A's first create finishes. The hook then makes no drain, release or context change, A's objects stay alive and drawn, `_hwnd` does not move to B, B's chain stays filtered, and A's next chain keeps the menu. The window is asked again under the overlay's locks (`DestroyVulkanObjects(false, hwnd)`, `1dca628b`). |
| present of a non-menu swapchain passes through | A present whose first swapchain is not the menu's (with the menu's chain second) is passed on unchanged, and no Vulkan or ImGui call is made. |

## Documented expectations: recorded, not decided here

These print `EXPECTATION` and assert what the code does today. A change to them is then a decision
someone makes, not an accident.

- **A hidden splash keeps the menu.** `OnOtherWindow` asks only `IsWindow()`, and a hidden window is still
  a window. The menu stays on a splash the game hid but did not destroy, and the main window's presents
  carry no menu until the splash is destroyed. No policy decides what should move it (visibility, size,
  recency).

## RED before the fix

At `294d1749` this suite fails 13 checks:

- In the concurrent attach, B drains the device, tears A's generation down, shuts the context down and
  re-inits it on B. The menu moves to the loser.
- The free-running rounds fail too: a natural interleaving hit the race within the first few rounds.
- Both destroyed-window cases, and every migration in the free rounds, call `ImGui_ImplVulkan_Shutdown` a
  second time. `DestroyVulkanObjectsLocked` (ours, `47138ca0`) had already shut the backend down before
  `CreateSwapchain`'s window-change block (upstream's) called it again unconditionally.

The fix re-checks `OnOtherWindow` under the locks and guards that second shutdown with
`_vulkanBackendInited`. Removing either one turns the suite red again.

## RED before the surface hook fix

At `2dcbbf40` the surface race case fails 4 checks: the losing surface hook drains the device, releases A's
whole generation and captures window B, and A's menu is dark until A's chain is recreated. The fix asks
`OnOtherWindow` again once `DestroyVulkanObjects` holds both locks; making that re-check `false` turns the
same 4 checks red again.

## What it does not cover

`DeviceCreated` maps only queues created without flags: a protected queue comes from `vkGetDeviceQueue2`, and
`vkGetDeviceQueue` on one is invalid (VUID-vkGetDeviceQueue-flags-01841). Upstream's own `DeviceCreated` skipped
them; ours did not until 2026-10-29. The case gives family 0 one plain and three protected queues and fails if the
menu asks for any index past the plain one.

These are fakes. No Vulkan driver, GPU, Wine, validation layer, real ImGui backend or game is involved.
A green run is not evidence about rendering, pacing or a real multi-window title. The fake device has
one graphics family, so the cross-family and pool-move paths are left to `tests/vulkan-overlay`, which
runs them on real Vulkan under Wine. The race cases prove the ordering of this translation unit's own
locks. They do not cover MenuOverlayDx initialising the same `MenuCommon` context from a DXGI present.
No title is known to be affected, and no crash from the double shutdown has been observed in a game.
It is a source-level defect that the fake turns into a fault.
