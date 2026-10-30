# NGX shutdown adapter

`python3 tests/nr-shutdown/run.py` compiles the production core-shutdown typedefs,
D3D11, D3D12 and Vulkan adapters and public-signature getters with host fakes under ASan/UBSan.

The D3D11 and Vulkan fixtures also compile the real raw slot and loader assignment, including its cast.
The Vulkan fixture also compiles both consumers of the public getter: the SDK input's
`NVSDK_NGX_VULKAN_Shutdown1` with the `NVSDK_NGX_VULKAN_Shutdown` it ends in, and the NR present host's
`ShutdownPresentCore`. It runs them before the type checks, so a one-argument slot fails on behaviour.
The fake core writes to the mandatory second argument, verifies its initial value and the forwarded
device, and returns success/failure codes. Tests verify that results are preserved, uninitialized or
missing entries are not exposed, and a null device selects the all-devices API instead of dereferencing
null inside the core. The public callable retains the SDK's one-argument type.

The same harness compiles `CustomNgxModule` and `ShutdownCustomDx12` from production:
uninitialized runtimes are inert, all-devices shutdown takes priority, the fallback calls the
one-argument SDK entry, missing entries are tolerated, and the private core slot is never used.
This preserves upstream's public-SDK contract for custom DLLs; it does not inspect arbitrary DLLs.

These are host behavior checks; the driver ABI is established separately through binary inspection
and the prior D3D12 Proton loopback and the D3D11 binary inspection. This fixture does not run the fixed DLL.
See `docs/dlssnr/design/ngx-shutdown-order.md` for that evidence.

`vk_shutdown_evidence.py` runs nothing: it judges one run of an existing `tests/vulkan-present-host` case
from the executor's receipt (run id, case folder, prefix, exit code, hashes of the facts, log, loader trace and
runner results): `host` for `nr-control`, `public` for `nr-route-quiet` run with `RETOQUE_VK_ROUTE_SR` (DLSS on).
It requires exit 0, the runner's PASS, the exact loaded DLL and prefix core, and the consumer's own debug lines in
order (bring-up or init, entry, core result 1, destruction); the host's "shut down" line alone is not a call.
`test_vk_shutdown_evidence.py` (run by `run.py`) checks those refusals on synthetic runs.
