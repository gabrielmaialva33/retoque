# The model's own network scale (`DLSSNR.ScalingRatio`): measured, not shipped

2026-10-20. Status: **negative result**. A key was implemented, measured, and removed again; this note keeps the
evidence so it is not tried a second time without a new reason.

## The lead

kkyleeb21's D18 fork, release 0.5.0 (2026-10-18), offers a "network ratio" for 310.9.16. Its notes say:

- at 0.5, NR time roughly halves;
- VRAM does not fall;
- 310.9.16 "uses the runtime's own scaling".

The model reads a float parameter `DLSSNR.ScalingRatio`. Both builds we have contain that string, along with:

- `DLSSNRComputeScalingRatioCallback`, which answers a quality mode with a ratio;
- the runtime's warnings "Network size changed for feature %u within feature capacity ... Recreate DLSSNR at
  the new scaling ratio".

So the network-scaling code exists in the model.

## What was measured

The test wrote `DLSSNR.ScalingRatio` through the float slot the host discovers (the same path through which
Intensity, Local structure and Local tone demonstrably take effect), before the feature's create and before every
evaluate. It ran the loopback moving scene (`tests/dlssnr-loopback`, production DLL, real NGX under Proton,
`LOOPBACK_MODEL` pointing at the 310.9.16 model, 276,162,560 bytes) with three settings:

| `ScalingRatio` | NR-on evaluate p50 | Per-frame NR-on vs NR-off MAE |
|---|---|---|
| not written | 1.611 / 1.618 ms | baseline |
| 0.5 | 1.602 / 1.611 ms | identical to baseline, all 16 frames |
| 0.85 | 1.619 / 1.609 ms | identical to baseline, all 16 frames |

In both runs:

- the log confirmed the write and a rebuild ("network scale 1.00 -> 0.50: the model is rebuilt");
- the model's own scaling-ratio query answered MaxQuality=1.0 and UltraQuality=0.85, and refused the other
  modes.

**The output is byte-for-byte the model's full-network output. The unmodified runtime ignores the parameter.**
0.85 is the one ratio the model itself offers, and it changed nothing either.

## Why D18 sees a difference

D18 patches the user's own copy of the runtime, on both 310.8 and 310.9.16, with byte-guarded profiles:

- `community/d18-installer/runtime_patch_source/` on branch `release/d18-050-20261018`, which holds profiles
  "310.8 layout family", "310.9.16 official" and "310.9.16 compat";
- `RUNTIME_COMPATIBILITY.md`: "NVIDIA runtime files are supplied by the user and patched locally".

For 310.8, the documented site is "Stop overwriting the retrieved `ScalingRatio` with 1.0". The unpatched
runtime reads the parameter, then forces it to 1.0 before `CreateNetwork`, which matches what we measured.

## Decision

- **Nothing ships.** A control that does nothing violates "no control shown for a thing that does not exist".
  The key, the write path and the menu slider were removed before merging.
- **What stays.** The two loopback hooks used for the measurement: `LOOPBACK_MODEL` and `LOOPBACK_NR_EXTRA`.
- **What would reopen it.** A runtime that honours the parameter unmodified (a future NVIDIA build), or a
  decision to patch the user's NVIDIA runtime locally the way D18 does.

  That second option is a policy question for the project, not a code question. D18's patch spec is GPL-3.0
  and could be ported. But it binary-patches code inside NVIDIA's DLL, it invalidates the signature, and it
  needs a byte-guarded profile per runtime build.
