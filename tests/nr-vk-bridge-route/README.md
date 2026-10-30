# Vulkan bridged-route NR

Run `python3 tests/nr-vk-bridge-route/run.py`. Pure Python: no compiler, no Wine prefix, no GPU.

A Vulkan upscaler backend whose feature derives from `IFeature_VkwDx12` runs on a D3D12 device, and
`IFeature_VkwDx12::Evaluate` already runs NR there. The Vulkan input must skip its own
`DlssNr::EvaluateAfterUpscaleVk` for every such backend, or the model runs twice per frame. `DLSS_on12`
was missing from that list until 2026-10-30.

| Rule | Read from |
|---|---|
| Every backend `FeatureProvider_Vk` creates a bridged feature for is in the Vulkan input's `bridged` list. | `case Upscaler::X:` blocks and `class ... : public IFeature_VkwDx12` |
| No Vulkan-native backend is in that list (its NR would never run). | the same |
| The bridge's `Evaluate` still calls `DlssNr::EvaluateAfterUpscale` (the list's premise). | `IFeature_VkwDx12.cpp` |

Each rule is first run against a copy of the real files with one violation planted. This proves the
routing decision only; one NR evaluation per frame in a real `dlss_12` Vulkan game is not yet observed.
