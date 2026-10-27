# Vulkan NR recording lifetime (V-06 / V-02)

Status: implementation design for the rendering-hardening batch approved in the conversation, 2026-10-27.

## Problem and boundary

The composition pass rotates eighteen descriptor sets and mapped UBO slices by modulo. A nineteenth recorded dispatch can overwrite data still named by an earlier command buffer before any submission. A successful device drain only proves submitted work complete; it does not retire executable or unsubmitted recordings. Retaining one old guide pair and then using elapsed-time DeferredRelease does not establish that boundary.

This change protects the NR composition slots and destruction/replacement of the after-upscale route's owned generation. It does not claim arbitrary application misuse, unknown command buffers, upstream scaler descriptor lifetime, or native-Windows/RTX-3060 runtime coverage.

## Recording registry

A fork-owned, standard-library-only registry associates device, pool and command-buffer handles with a monotonically changing recording epoch. Allocation must be observed, then a successful Begin opens a recording. A ticket identifies that exact epoch. Pinning is all-or-none and accepts only the current open ticket.

Successful Begin (implicit reset), ResetCommandBuffer and ResetCommandPool remove the previous recording's leases. FreeCommandBuffers and DestroyCommandPool remove records after forwarding. End preserves leases while closing admission. Submission and fence completion never release a lease: legal resubmission remains possible. Failed Vulkan operations leave the registry unchanged. Reused handles cannot admit stale tickets.

The existing Vulkan_wDx12 wrappers observe the application lifecycle, including exported and proc-address paths. The opt-in Vulkan present host also installs independent lifecycle observers through its existing multi-target detour mechanism, so marker/SkipHooks modes retain the same protection without enabling the old bundle. Host-owned command buffers explicitly report operations that use trampolines. Unknown command buffers remain inadmissible.

References removed under the registry mutex are destroyed after unlocking. This permits lease destructors to reenter and avoids inversions with the route mutex. Device invalidation removes all epochs only at a real device-destruction boundary or after marking resources abandoned.

Retirement performs no allocation, including default construction of empty STL containers. The registry map is lazy and pin batches are allocated only inside the guarded Pin path. A bulk Free detaches all records before running any deleter, so a callback that recycles another handle cannot have its new record removed by the old notification. Detached batches drain iteratively outside the mutex.

## V-06: bounded slots before recording

The eighteen-slot allocation stays bounded. A slot contains one descriptor set and its UBO slice and is exclusive to its recording. Complete fallible CPU-only guide validation first, then reserve the entire frame's composition budget before the first command (including feature-create work). Exhaustion declines NR before partial frame recording. A slot lease owns separate shared allocation metadata, so returning it never dereferences a destroyed shader object.

The after-upscale route needs at most four composition dispatches: encode, optional resample fallback, optional exposure meter, resolve. Reserve the required conservative budget; unused reserved slots remain leased until recording reset. The present route needs at most three. Its existing admission/ticket logic remains a separate bound, not evidence that a slot is free.

A refusal is observable with a bounded log; no default setting is changed. More retained recordings than the fixed pool can accommodate can skip NR until buffers are reset.

## V-02: quiescence before changing the generation

A token represents the entire active after-upscale VkState resource generation. Every NR recording pins it before any GPU work is emitted. All frame-size/working-size rebuilds, scaler-filter replacement, route release and owner hand-back require no older recording pins. While a change waits, do not acquire a new pin: subsequent resets can bring the generation to quiescence. Then the existing drain/failure handling and allocation paths may rebuild it.

The gate covers feature/parameter lifetime, composition/scaler passes, owned images including matched guides and retained pairs, query pool and meter/readback buffers. It must precede g_owner.FinishHandBack, so ownership is not given away while the previous feature remains referenced. Allocation failures may free objects demonstrably never recorded; failures after recording retain the generation pin.

This intentionally avoids creating overlapping NGX feature generations. A recording retained indefinitely keeps NR refused at a pending resize instead of causing unbounded retained allocations. The application owns its own texture lifetime.

At real device destruction, the existing bounded drain decides destroy versus abandon; recording epochs are invalidated as part of that boundary. A lost/unproven device uses the existing abandonment policy and never gains destructor calls merely because pins were removed.

## Discarded recordings and image layouts

CPU-side layout updates do not prove a recorded transition executed. Each recording starts fully overwritten route-owned images from UNDEFINED, preserving barriers within that recording; foreign images retain their existing explicit layout contract. The composition placeholder likewise records its transition in every recording, so discarding the first Prepare cannot suppress the next transition.

## Validation

- Behavioral host tests: delayed submission, executable resubmission, reset/free, pool reset/destruction, handle reuse, stale tickets, device invalidation, callback reentrancy.
- Slot tests: all eighteen immutable leases, nineteenth refused, multi-slot all-or-none reservation, reuse only after reset, outstanding lease outlives allocator.
- Generation tests: two old recordings hold resize/hand-back; refusal adds no pin; both resets permit change; partial recording failure retains pin; abandon has no GPU callback.
- Integration tests exercise production dispatch/hook slices with fakes where possible, plus real Vulkan validation/readback if the installed environment supports it.
- Full host suite, Release build, and available GPU fixtures are separate gates. Portable green is not native-GPU qualification.

## Specification basis

Vulkan requires reset/free/pool-destruction targets not to be pending. Reset removes implementation access to objects named by the old recording. These valid-usage rules support conservative retention until reset/free:

- https://docs.vulkan.org/refpages/latest/refpages/source/vkResetCommandBuffer.html
- https://docs.vulkan.org/refpages/latest/refpages/source/vkResetCommandPool.html
- https://docs.vulkan.org/refpages/latest/refpages/source/vkFreeCommandBuffers.html
- https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyCommandPool.html
