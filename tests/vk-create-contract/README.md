# Vulkan device-creation contract

Run `python3 tests/vk-create-contract/run.py` (one case: `run.py <case>`; `--no-mutations` skips the
control). The runner cuts the production device-creation path out of the source and compiles it, unchanged,
into one translation unit with `fakes.h` and `cases.cpp`, against the real Vulkan headers in `external/vulkan`,
with g++ C++20, ASan/UBSan and `-Werror`:

- `OptiScaler/hooks/Vulkan_Hooks.cpp`: `hkvkCreateDevice` (whole), `TimelineSemaphoresEnabled`,
  `TrackPhysicalDevice`, `GetPhysicalDeviceApiVersion` and the globals they use;
- `OptiScaler/spoofing/Vulkan_Spoofing.cpp`: the feature helpers, `VulkanDeviceFeatureState` and
  `VulkanSpoofing::hkvkCreateDevice` (whole), with the header's declarations;
- `retoque/dlssnr/DlssNr_VkExtensions.h`, through its include root, for the NR block inside the hook;
- `OptiScaler/State.h`: `ScopedSkipVulkanHooks`, `ScopedCreatingD3DDevice` and the per-thread
  `creatingD3DDevice` declaration.

The rest is fake: logging, `Config`, `State`'s other fields, the FG output (a no-op `PrepareDevice`), the menu,
Hudfix and deferred-release calls after the create, and the driver. The fake driver answers
`vkEnumerateDeviceExtensionProperties`, `vkGetPhysicalDeviceFeatures2` and `vkGetPhysicalDeviceProperties` from a
per-GPU table (three GPUs: an NVIDIA-like one with everything, a minimal one without NVX, push descriptors or
Device-scope memory model, an AMD-like one with Anti-Lag and only the EXT buffer_device_address). At
`vkCreateDevice` it deep-copies the exact extension list and feature chain it was handed, checks them against the
rules below, and reads every pointer again just before returning.

## The contract

Every create, in every case, is checked for:

- **the game's request unchanged after the call**: the `VkDeviceCreateInfo` byte for byte, the extension pointer
  array, the strings, and every structure of the `pNext` chain with its feature bits;
- **every pointer alive until the driver returns**: the request read at entry and again before returning must be
  the same, under ASan (`detect_stack_use_after_return=1`), so storage freed, reallocated or reused while the
  driver runs is an ASan report or a difference;
- **no rule broken that the game had not broken itself**: an extension the device does not offer, an extension
  listed twice, KHR and EXT buffer_device_address together (03328), a structure type twice, `pEnabledFeatures`
  beside `VkPhysicalDeviceFeatures2` (00373), a feature bit the device lacks, a promoted structure beside its
  `VkPhysicalDeviceVulkanNNFeatures` (02830, 06532), a promoted structure enabled below its core version without its
  extension, an aggregate structure below its version, `VK_KHR_push_descriptor` with
  `VkPhysicalDeviceVulkan14Features::pushDescriptor` off, `vulkanMemoryModelDeviceScope` without
  `vulkanMemoryModel`, and core/KHR `bufferDeviceAddress` beside `VK_EXT_buffer_device_address` (04748). A game whose
  own request is already invalid for its version is exempt from this one check, and so is a feature structure left
  behind when an extension the device lacks (one a spoofed enumeration showed the game) is taken out of its list;
- **nothing the game asked for and the device has is dropped**, except `VK_EXT_buffer_device_address` given up for
  the KHR one;
- **under `ScopedSkipVulkanHooks` or on dxvk/vkd3d-proton's create (`ScopedCreatingD3DDevice`)** the feature chain
  reaches the driver as the game sent it, and FG prepares nothing.

The matrix most cases run (`Matrix` in `cases.cpp`) is the instance API version known as 1.0, 1.1, 1.2, 1.3, 1.4
or unknown to the hook, times the device's version 1.0 to 1.4, times the three GPUs, times ten request shapes (bare,
`pEnabledFeatures`, the usual Features2 -> 1.1 -> 1.2 -> 1.3 chain, Features2 -> 1.2 with bufferDeviceAddress, an
unknown structure ahead of 1.2, individual 1.1-era structures with their extensions, EXT buffer_device_address with
its structure, a 1.4 structure with pushDescriptor off with and without the extension, and the names a spoofed
enumeration shows), times the toggles each case sets: NR on/off, `VulkanExtensionSpoofing` on/off, skip scope,
dxvk/vkd3d-proton path, FG.

| Case | What it adds |
|---|---|
| `game-request-unchanged`, `-fg` | the matrix with NR and spoofing toggled, without and with an FG output; prints how often the game's own structures were written during the call (and restored, which is checked) |
| `pointers-live-until-return` | the matrix under all 32 toggle combinations |
| `interleaved-native`, `interleaved-d3d` | A-01: device A's create held inside its driver call -- before the driver reads anything, and while it reads -- while device B creates start to finish on another thread. A and B must each receive exactly what they receive alone. Native path and dxvk/vkd3d-proton path |
| `interleaved-mixed` | the same with A and B on different paths, with NR on both, and with a skip scope on A's thread only |
| `antilag-crosstalk` | A (no Anti-Lag) held right after its features query while B (Anti-Lag) creates: A never asks for `VK_AMD_anti_lag` |
| `capabilities-native`, `capabilities-nr` | the matrix; with NR on, no model extension the device lacks is requested, and the NVX pair reaches a device that has it |
| `push-descriptor-off`, `-supported` | 1.4 device: the extension never beside pushDescriptor off; turned on with the extension where supported |
| `bda-never-both` | the matrix with NR, spoofing and the dxvk/vkd3d-proton path toggled: never KHR and EXT together |
| `memory-model-device-scope` | a device without Device scope never gets it requested; one with both gets both, at 1.1 and 1.3 (e35f1c76) |
| `api-versions` | the matrix with NR, spoofing and FG; below 1.2 no promoted structure is added without its extension |
| `enumeration-fails-native`, `-incomplete-native`, `-fails-d3d`, `-incomplete-d3d` | the device's extension list failing at the count or the data query, or one short at the first data query: nothing optional added while the list is unknown, nothing the game asked for that the device has dropped |
| `nr-off-on` | NR off adds no model name; NR on adds only the model's names, no feature; NR on reaches dxvk/vkd3d-proton's device too |
| `skip-scopes-nested` | the production `ScopedSkipVulkanHooks`: nested scopes, the inner one's end, another thread unaffected, and the matrix inside a scope |
| `extension-spoofing-off-on` | spoofed names the device lacks removed with spoofing on (both paths), the game's list unfiltered with it off |
| `d3d-path` | the production `ScopedCreatingD3DDevice`, and the matrix on dxvk/vkd3d-proton's create: only NR adds, only the model's names |

### The A-01 control

By default the runner also builds the same unit with the two device-create lists put back to function-static (as
they were before d391a944) and requires `interleaved-native` and `interleaved-d3d` to fail with them. They do: the
held create reads B's names, or freed memory.

### The probe

`nested-create-in-driver` is run and reported, not counted: a create made **on the same thread** from inside the
driver's call for an outer create reuses the per-thread list the outer request still points at, and the outer
request changes under the driver. d391a944 made the lists per thread, not per call. No path is known that creates a
device on the same thread from inside the driver call through OptiScaler's hook (layers call down their chain, not
the loader's export), so this is the boundary of that fix, not a failure. Per-call storage owned by the caller
would close it.

## Found by this suite

- **The unknown-instance-version path put promoted structures beside the game's aggregate ones.** With the
  instance's version unknown to the hook (its instance created before the hooks, or by OptiScaler's own
  Streamline), the spoofing took the 1.0 path and added `VkPhysicalDeviceShaderFloat16Int8Features` and the rest
  beside a game's `VkPhysicalDeviceVulkan12Features` (02830), and the 1.3 ones beside its
  `VkPhysicalDeviceVulkan13Features` (06532). Fixed by taking the version a request's own aggregate structures
  imply.
- **dxvk/vkd3d-proton's extensions all dropped when the device's list could not be read.** The filter for spoofed
  names on that path ignored the enumeration's result: a failed count query left the list empty and every
  extension, `VK_KHR_swapchain` included, was removed. Fixed by keeping the request as it is when the list is not
  complete, as the native path already does.

## Observed, not failures

- `VulkanDeviceFeatureState` writes the game's own feature structures in place when they sit behind one its copier
  does not know (`VkPhysicalDeviceVulkan11Features` and `VkPhysicalDeviceVulkan14Features` included), and the FG
  output's timeline-semaphore request writes the game's `VkPhysicalDeviceVulkan12Features`; both are restored after
  the call, which every create checks. A game whose chain lived in read-only memory would fault there.
- `State::vkAntiLagSupported` is process-wide and written by every create: two creates on different GPUs that
  overlap can leave the one with Anti-Lag without `VK_AMD_anti_lag` (`antilag-crosstalk` prints it). The one
  without it never asks for it, because the spoofing checks the device's list.

## What this does not cover

No Vulkan runs. The fake driver checks the rules listed above, not the full valid usage of `vkCreateDevice`, and it
believes its own feature tables. The FG output's `PrepareDevice` (queues, DLSS-G's own extensions and features),
`hkvkCreateInstance`, the post-create hooks and the menu are fakes. Whether a real driver or the validation layer
accepts these requests is for a run under the validation layer.
