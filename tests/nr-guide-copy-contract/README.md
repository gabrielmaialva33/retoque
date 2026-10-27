# D3D12 guide copy and first-device contract

`python3 tests/nr-guide-copy-contract/run.py` compiles production `TypedGuideFormat`,
`CreateGuideClone`, copy-compatibility helpers and `ReadableGuide` with host D3D12 fakes,
ASan and UBSan. The fake independently classifies DXGI families and observes actual
creation attempts, copies, barriers and parked resources. It rejects cross-family and
cross-device copies. This suite also runs `owner-run.py`.

The guide scenarios cover changed extents/mips/arrays/MSAA count and quality, changed
format family or device, legal alternative reuse, illegal packed-depth-to-R32 fallback,
failed FORMAT_INFO queries and mismatched plane counts. Matching two-plane copies and
matching MSAA descriptors remain accepted; unavailable evidence refuses recording.

`owner-run.py` extracts the production retained owner, `EnsureCompose`, the real Dispatch
prefix before tracking/timing/barriers, and the CaptureGuides prefix before ring effects.
It verifies first-device AddRef/repeat/no-extra-AddRef, explicit second-device refusal
without replacing the old pass, pass/list/resource/exposure consistency, and capture
mip-zero plane availability and single-sample quality. The owner is retained for the
process session. Changing device requires restarting the process: partial Shutdown does
not authorize hot rebinding of residual capability and renderer caches.

Present capture intentionally copies mip zero of depth plane zero, not stencil. A donor
may have extra mips or a stencil plane; numeric source indices of other planes are not
used as destination indices. Full-resource typeless guide copies require identical whole
shape, sample count/quality, DXGI family, plane count and device.

These are host contracts, not proof of GPU completion, NGX format acceptance, visual
quality, Windows hardware behavior or latency. Native Windows/RTX3060 qualification
remains pending. `nr-before-upscale` separately tests actual entrypoint refusals before
state normalization and scratch allocation, while retaining its strict state-restoration
simulation and existing small format vocabulary.
