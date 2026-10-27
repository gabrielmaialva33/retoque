# D3D12 resource ownership and guide-copy contracts

Status: implementation of the first approved rendering-hardening batch. Native host tests and the final Release build are tracked in `docs/superpowers/reviews/2026-10-27-rendering-hardening.md`.

## Device identity

The NR compositor and its associated caches accept one D3D12 device per process. `NrDeviceOwner` retains the first accepted identity with one explicit `AddRef`; repeated acceptance adds no references. Admission of a different device fails before cache replacement or recording. Production access is serialized by the existing NR mutex.

This identity deliberately survives the current partial `Shutdown`: capability parameters and other device discriminators already outlive that operation. Resetting only the new owner would incorrectly imply that the remaining caches support a device switch. A process restart is required to enable NR on another device. The owner destructor returns only its additional identity reference; this change adds no forced destruction of pending GPU resources.

`EnsureCompose` returns an explicit success value, consumed by all callers. Direct dispatch checks the command list, colour, depth, motion, output and optional exposure against the pass device before tracking, timing, barriers or writes. Before-upscale admission checks devices before tracking, parking or allocating a clone. Exposure is checked before read-state transitions. The actual Present submission queue must match the backbuffer device before obtaining a command list.

## Whole-resource guide copies

`GuideCopyCompatible` verifies full shape, mip and array counts, known DXGI type family, sample count and quality, and successful nonzero/equal plane counts from `D3D12_FEATURE_FORMAT_INFO`. `DeviceChildBelongsTo` validates the command list, source and any reusable clone and releases temporary `GetDevice` references.

Microsoft requires identical dimensions and compatible type groups for `CopyResource`, and matching multisample count and quality. The former fallbacks from R24G8 or R32G8X24 families to R32_FLOAT violate that contract and are removed. Legal typed alternatives within the original family remain available. A clone of a changed format/device is retired through the existing parking policy rather than immediately released.

## Present capture

Present capture intentionally copies mip zero of depth plane zero and motion plane zero. It does not copy the whole source allocation. Its separate contract accepts extra donor mips and a depth/stencil allocation with separate planes, while requiring known nonzero plane availability, identical formats, a single array slice, a one-mip clone and single sampling with quality zero. Depth/stencil copies keep a full null source box and zero destination offsets.

Command-list and resource ownership are checked before ring mutation. The capture ring retains its existing device discriminator rather than becoming globally bound to the compositor owner. Actual allocated clones are checked again. If that post-allocation check rejects a selected slot, the slot is invalidated before returning so that its previous stamp cannot remain published.

## Verification boundaries

The host fixture compiles the actual production helpers and pre-recording code with controlled D3D12 objects. Its 29 cases cover accepted copies, legal typed fallback, two-plane formats, shape changes, incompatible type families, plane-query failure, plane mismatch, mixed devices, retained owner identity, dispatch refusal, capture admission and post-allocation slot invalidation. RED runs reproduced the original defects before implementation. The existing before-upscale fixture exercises the surrounding state/barrier and recording behavior.

These tests establish admission, references and copy contracts. They do not establish native Windows runtime correctness, RTX 3060 behavior, NGX format support, visual quality or performance. GPU execution evidence and its exact limitations are recorded separately. Shader code, bytecode, model selection and the existing resource parking policy are unchanged by this adjustment.

## Primary API references

- [ID3D12GraphicsCommandList::CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource)
- [ID3D12GraphicsCommandList::CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion)
- [D3D12_FEATURE_DATA_FORMAT_INFO](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_format_info)
