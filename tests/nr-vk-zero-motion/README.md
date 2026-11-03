# nr-vk-zero-motion

Host suite for zero-motion detection on the Vulkan after-upscale route
(`docs/dlssnr/design/zero-motion-detection.md`, "Vulkan").

Run: `python3 tests/nr-vk-zero-motion/run.py` (needs `g++` with C++20, ASan and UBSan, and the Vulkan headers in
`external/vulkan/include`).

`run.py` does three things.

1. **Source guards.** The shader's `VK_MODE` block comes first and the D3D12 declarations under its `#else` are
   textually what they were; `DlssNr::MotionProbe::Vk::Push` equals `ProbeParams` and the D3D12 `Params`; bindings 0
   and 1 are sampled, 2 the `rgba32f` grid; the SPIR-V is optional (`__has_include`) and the build comment names its
   command. In `EvaluateAfterUpscaleVk`: the key read once; consume, plan, policy, reservation and recording each once,
   each behind `ZeroGuideReset`; the plan after the guide decision and before the composition reserve, the reservation
   after the generation pin and the meter's and before `BeginRouteRecording`, the record after the meter and before the
   model, the policy reset before the evaluate, with its own counter; switched on again, pending probes discarded. No
   wait function in the probe's route functions, the pass or the route state. `ForgetMotionProbe` in `ShutdownVk(false)`
   and a lost device's `AfterUpscaleDeviceGone`, the readback metadata dropped before `ForgetDevice` on a drained one,
   `DestroyMotionProbe` only after `FreeRouteObjects`' busy check; the probe made lazily; the three registry `Begin`
   observers passing the flags on; the files in the project once each.
2. **Cases** (`cases.cpp`), compiled with the production `retoque/dlssnr/DlssNr_VkMotionProbe.h` and the real
   `VkMeterReadback::Ring`, `VkRecording::Registry` and `DlssNr_MotionProbe.h`, the production
   `retoque/shaders/dlssnr/DlssNr_MotionProbe_Vk.cpp` against Vulkan fakes that record every command, and the route's
   probe functions and state extracted verbatim from `retoque/dlssnr/DlssNrFeature_Vk.cpp`. "The GPU ran the copy" is a
   grid written into the slot's mapped buffer; "the game reset its command buffer" is the registry seeing it. No wait
   function is defined, so a probe path that called one would not link.
   - the pure rules: the push block's order, the shape, the dispatch covering the grid, `Reuse` from `Begin`'s flags;
   - one probe's command stream: the slot's grid from `UNDEFINED`, the game's views in `SHADER_READ_ONLY_OPTIMAL`, the
     regions pushed, 4x4 groups, the copy into the slot's own buffer, the host barrier, no barrier on the game's images;
   - a baseline and two pans of an all-zero field: None, None, Dead; the policy reset then, not on a frame the game
     reset; one non-zero sample: Alive;
   - the sentinel: a slot whose buffer still holds an older valid grid, handed to a recording that is discarded
     unexecuted, feeds nothing (without the sentinel that old grid would form a false verdict);
   - retirement out of order fed in recording order; a probe older than the newest fed freed unread;
   - saturation (the fifth skipped, said once), elapsed evaluates and resubmittable recordings freeing nothing, the
     verdict acting up to 64 evaluates past its newest probe and dropped after;
   - **Dead on device A, then a stable failure on device B: no policy reset on B**, the verdict dropped with A, every
     refused evaluate counted and the reason said once; and provenance alone refusing A's verdict on B;
   - a refusal of the game's views dropping the verdict for good; recordings begun with `SIMULTANEOUS_USE` or with
     unobserved flags refused; two live recordings writing two grids and two buffers, one re-executed recording read
     once;
   - the shape fed being the shape reserved; forgetting with live pins (no call, nothing read); a drained teardown;
     a lost device, an unsupported grid format and an unallocatable buffer while the pass is made;
   - the three ends allocating, calling and saying nothing on a route that never had the key on;
   - probes recorded before a refusal dropped the verdict, retiring after it, freed unread (they would otherwise form
     Dead again); the verdict forming again only from probes reserved after the drop;
   - a descriptor-set layout, pipeline layout or pipeline create failing with a poisoned output: only the handles that
     were created are destroyed, each once.
3. **Mutants.** Ten planted violations, each of which must make its named case fail (no sentinel, slot order, no
   provenance, a simultaneous recording admitted, a refusal, staleness or a device change keeping the verdict, a
   dropped verdict keeping its pending probes, a failed layout create's output stored), and seven
   that must make a guard fail (consume or plan without the key, the reservation after the first command, a wait in
   consume, `ShutdownVk` keeping the probe, two push fields swapped, the policy reset in the game's counter).

What it does not cover: the shader on a device, a real readback, the game's layouts, Proton or native Windows, a game.
Those are the GPU harness and the in-game checks named in the design note.
