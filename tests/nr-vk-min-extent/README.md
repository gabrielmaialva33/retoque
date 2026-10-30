# Vulkan NR minimum extent (host)

```bash
python3 tests/nr-vk-min-extent/run.py
python3 tests/run_all.py --only nr-vk-min-extent --strict
```

A model built at 1x1 hung the GPU on its first submit in DLSS5VKLayer (`common/shm_protocol.h`, Linux NVRM Xid 109,
CTX SWITCH TIMEOUT). `kDlssNrMinExtent` (64, `retoque/shaders/dlssnr/DlssNr_Common.h`) is the floor on both APIs;
`DlssNr::BelowMinExtent` beside it is the one decision the Vulkan routes ask.

## Covered

- `test.cpp`, compiled with ASan/UBSan against the real Vulkan headers: every one of the frame and working sizes at
  0, 1, 63, 64, 65 and 3440 alone; probe windows; a window made small by the working scale; static asserts for 1x1
  and 64. The present host's `TransportStats` counting three `NrSkip::TooSmall` refusals and `NrText` saying
  `refused 3 (too small 3)` with the first refusal's reason.
- Source guards: the constant is defined once; the Vulkan file compares only through the decision; the after-upscale
  route asks it before the forwarder, NGX, the token or any image, counts every refused frame (`g_vkTooSmall`) and
  logs the reason with the count (first, then at most every 10 s); the present host's `Admit` refuses with
  `NrSkip::TooSmall` before deciding or requesting a build; the builder refuses a key below the floor before the
  forwarder, NGX or any allocation; D3D12 keeps its check on the same constant.

## RED / GREEN (2026-10-30)

Before: `test.cpp` did not compile (`'BelowMinExtent' is not a member of 'DlssNr'`) and 18 guards failed (the route
logged the first refusal once and counted nothing; both Vulkan sites compared inline; the builder had no floor of its
own). After: the suite passes.

## Not covered

No Vulkan, NGX or GPU runs: that a 1x1 or 63-pixel swapchain is carried unchanged in a game is for a game run.
