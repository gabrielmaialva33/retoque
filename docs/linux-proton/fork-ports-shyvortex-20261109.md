# Proton swapchain scope, Reflex ownership and forwarder version (2026-11-09)

Four narrowly adapted ports on baseline `cc79faac`, from sibling GPL-3.0 forks. No model, shader, config,
GPU resource-lifetime or ABI change. These general hook corrections apply with NR enabled or disabled;
they do not add an NR-only hook or allocation. Runtime qualification remains open.

## Vulkan swapchain scope is local to the calling thread

Source: [ShyVortex/OptiScaler-NR-PreSR-MFG a7bb72b0](https://github.com/ShyVortex/OptiScaler-NR-PreSR-MFG/commit/a7bb72b09e23670bc65bd4d3ca1ac1649cb46463), GPL-3.0.

`State::vulkanCreatingSC` is now `inline static thread_local`. A Vulkan original call can reenter DXGI
on the same thread; those internal swapchains must still bypass wrapping. DXVK's present thread must
not make a simultaneous game-thread recreation bypass wrapping and lose its bridge/FG/borderless path.
The RAII save/set/restore logic is unchanged, including nested scopes.

Pre-edit audit (`rg -n vulkanCreatingSC OptiScaler retoque`, then the RAII call sites): **13 references**
in this baseline, not the 18 anticipated in the brief. One declaration, three accesses in the scope
(previous-state read, set, restore) and nine DXGI decision reads:

| Reader in baseline | Meaning of true |
|---|---|
| `DxgiFactory_Hooks.cpp:401,778,1193` | `CreateSwapChain`, `ForHwnd`, `ForCoreWindow`: call the original/factory and return without wrapping |
| `DxgiFactory_Hooks.cpp:1308,1593,1906` | Corresponding `DLSSGCreate*` entrypoints: the same bypass |
| `DxgiFactory_WrappedCalls.cpp:80,448,855` | Wrapped factory entrypoints: the same bypass |

Only `ScopedVulkanCreatingSC` writes the flag. Its two callers are `hkvkQueuePresentKHR` and
`hkvkCreateSwapchainKHR`; each scope spans a synchronous original/FG call on the entering thread.
No consumer polls, waits, publishes work, or uses the flag as a cross-thread ownership/retirement signal.
All nine consumers express recursion suppression. Full source search found no other accesses. This is
a source audit, not proof of undocumented driver thread dispatch; a cross-thread internal DXGI create
would require explicit identity propagation rather than a process-wide boolean.

## DXGI owns Reflex updates for DXVK/vkd3d-proton presents

Source: [ShyVortex/OptiScaler-NR-PreSR-MFG e374fa32](https://github.com/ShyVortex/OptiScaler-NR-PreSR-MFG/commit/e374fa3279621ad58fd8e6c619857dddd5f5a3bc), GPL-3.0.

`hkvkQueuePresentKHR` calls `ReflexHooks::update(false, true)` only for `!dxgiBacked`. This tree already
classifies the original present caller (including the NR present-host detour), so it reuses that local
classification rather than importing the sibling's GPU/swapchain-API predicate. DXGI retains its update
with the actual FG state; native Vulkan retains one update. The fallback sleep is unchanged.

## Async marker expiry requires prior game async markers

Source: [ShyVortex/OptiScaler-NR-PreSR-MFG f732bbe2](https://github.com/ShyVortex/OptiScaler-NR-PreSR-MFG/commit/f732bbe2a6ab09883176856d25248a42e8444be6), GPL-3.0.

`processGameMarker` computes the RTSS high-bit classification before expiry. Clearing the generated-frame
count now requires a non-RTSS marker, nonzero async history and the existing strictly-more-than-ten-frame
age. Games with no async history and RTSS markers no longer trigger that reset. The later notification
reuses the same classification. Explicit FG-off setters and marker forwarding are unchanged.

## Forwarder release identity

Source idea: [janblade/OptiScaler-F5-DLSSNR-Multipass bb228e0a](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/bb228e0a26e7d8c88bbb9d285fdd15c402499fd4), GPL-3.0.

`dlssnr_forwarder.rc` includes the canonical `OptiScaler/resource.h`. Both fixed numeric version fields
use its macros, exactly as `OptiScaler.rc`; no release number is duplicated. MSBuild and MSVC CMake both
include the resource. It does not force `_DEBUG` and does not depend on generated build stamps.

The forwarder's FileVersion/ProductVersion strings use the stable four-part release number.
OptiScaler's descriptive ProductVersion string still includes dev/commit/date metadata. A desktop app
should compare the fixed numeric file/product fields for release matching. Matching release versions
alone do not prove exact build identity or ABI compatibility; retain the ABI handshake and hashes.
The source's packaging enforcement is not part of this port.

## Validation and limits

- `proton-reflex-contract`: seven production-slice cases under ASan/UBSan and four defect controls.
  Before the fixes, interleaved threads, DXGI update ownership, missing async history and RTSS failed;
  nested/native/grace-boundary controls passed. After the fixes all pass, and each restored defect is rejected.
- `nr-forwarder-version`: absent resource failed before the change. Afterward, build wiring and matching
  numeric file/product versions pass, including a synthetic major-version bump and four rejected mutations.
- Final full host tier: `python3 tests/run_all.py --tier host --jobs 4 --strict`, in a user scope with
  `MemoryHigh=14G`, `MemoryMax=16G`, `MemorySwapMax=2G`, `OOMPolicy=kill`: **132 PASS, no failures/skips**.
  The first run had 131 PASS and one replay-format failure: its scandir guard rejects an ancestor named
  `artifacts`, which was in the chosen TMPDIR. The unchanged suite passed with a TMPDIR outside that
  component, then the complete tier passed with that environment.
- `python3 tools/check_docs.py`: PASS (350 Markdown files and catalog). Clang-format 22.1.8 on the touched
  C++/headers and `git diff --check`: PASS.
- `./build-local.sh Release`, directly, with file-redirected logs: PASS, both DLLs produced by MSVC under
  Wine. Warnings observed: C4838 in untouched `Streamline_Hooks.cpp`, C4018 in untouched `IFeature.cpp`,
  and linker LNK4098 (`LIBCMT`). No warning-free-build claim.
- `pefile` read the actual two x64 DLLs: both fixed file/product versions are **10.0.0.1**, both
  FileVersion strings are `10.0.0.1`, and the forwarder has its own correct OriginalFilename.
  The main ProductVersion string includes `cc79faac` and build stamp `20261109_110133`; the forwarder
  uses the stable release string as described above. CMake wiring is source-checked, not built here.

SHA-256 of the locally built pair (baseline `cc79faac`, dirty worktree):

| File | SHA-256 |
|---|---|
| `OptiScaler.dll` | `664335b88070ff3040a4e2fd7b9163ac61758d0ee19ba86793e5b50b09b353b9` |
| `nvngx.dll_dlssnr.dll` | `b5a1bc7ec21d8dcec168c9c9dbe70243b51db2bc78fd25569d3ad20e4733c499` |

No game was launched, no installed DLL was replaced, and no GPU/Proton/native-Windows behavior, FPS,
image quality or F11/Alt-Tab outcome was measured. Host results and a successful build cannot qualify them.
