# Proton swapchain scope and Reflex ownership

`python3 tests/proton-reflex-contract/run.py` compiles the production `vulkanCreatingSC` declaration and
`ScopedVulkanCreatingSC`, the Vulkan present's Reflex update statement (including its guard), and the start
of `ReflexHooks::processGameMarker` against minimal fakes under ASan/UBSan.

Seven cases cover nested restoration, two ordered threads entering and exiting in overlapping scopes,
DXGI-backed presents (no Vulkan Reflex update), native Vulkan (one update per present), no async marker
history, RTSS's high-bit IDs, and the ten-frame grace boundary. Four controls restore each defect and must
fail with its specific assertion. Semaphores keep the global-flag control free of data races.

The slices execute the production decisions, not the complete Windows hooks. The caller classification
and both RAII call sites have source guards. No Vulkan, DXGI, Wine, real NVAPI, game, timing or pixels run.
See [the port record](../../docs/linux-proton/fork-ports-shyvortex-20261109.md) for the reader audit.
