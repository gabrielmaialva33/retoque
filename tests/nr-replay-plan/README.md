# nr-replay-plan

Host checks for `tests/dlssnr-loopback/run_replay.py`'s capture loading and the replay plan it hands the harness
(`docs/dlssnr/design/offline-replay-seam.md`, "Plan v3").

- A v1 capture gives the v1 surfaces (R16G16B16A16_FLOAT output with unordered access, R32_FLOAT depth, R16G16_FLOAT
  motion) and a full-extent, unscaled geometry. Those are the values the v1 harness set, and they keep the replay
  baselines bit-exact.
- A v2 capture carries each plane's format, source format, flags and extent, and each frame's create flags, Ray
  Reconstruction placement, MV scale, render subrect and subrect bases. Each frame line has the token count
  `replay_nr.h`'s `fscanf` expects.
- Geometry the pass clamps (a base or subrect past its plane, a 0x0 subrect) is carried as recorded, not refused.
- Refusals, each with its reason:
  - a capture that is not finalized;
  - an output without unordered access;
  - a guide that denies shader resource views;
  - a shape that changes mid-capture;
  - half a render subrect (one dimension zero);
  - create flags that change;
  - a digest mismatch;
  - a format outside the recorder's whitelist, or the wrong pixel size for it;
  - an exposure texture;
  - nonfinite parameters;
  - presents that do not move forward;
  - jittered colour.

What it does not cover: the harness itself, which runs only under Wine. The replay baselines
(`tests/dlssnr-loopback/README.md`, "In-flight lifetime gate") and the record check cover it.

```bash
python3 tests/nr-replay-plan/run.py
```
