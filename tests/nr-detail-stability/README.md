# NR detail stability

```bash
python3 tests/nr-detail-stability/run.py
```

Compiles the shader's own rule (`dlssnr_detail_stability_rule.h`) and `DlssNr_DetailStability.h` with g++ C++20 under
ASan/UBSan and `-Werror`, runs `cases.cpp` over synthetic frame sequences, then rebuilds with 25 one-line mutants, each of which
has to fail the cases. Design: [detail-stability.md](../../docs/dlssnr/design/detail-stability.md). This is the rule and where
it is called, not the shader on a GPU and not the picture.

## What the cases cover

- the configured blend (NaN reads as the default), every refusal, and the gate: first frame, a second view in one present, a
  changed signature, the model's reset, a gap, a frame that was not recorded, a refusal in between, no present count;
- the edit and its inverse; black does not blow up;
- no history or alpha 1: the model's answer exactly; a history the host says is no good is not used even when it looks perfect;
- what is stored is a half and what is shown is made from it;
- a still scene whose edit boils every frame: the filtered answer flickers under 0.45 of the model's, the mean light unchanged;
- a pan with the game's vector: filtering with the vector beats filtering without it and beats the model alone;
- the clip: an edit the current 3x3 no longer holds does not persist;
- rejection: a cut passes the model's answer whole; an input change inside 0.02 is not one; the band is near binary (nothing
  at the bottom, whole at 0.10, monotonic between); the 3x3 range is judged and not the pixel; off the picture, a texel that
  stored nothing (also at 1% of the bilinear weight) and a vector that is not a number reject;
- the dead end side by side: an object leaves a texture; a plain blend of the old answer keeps 0.1 or more of the old edit,
  this pass shows the new answer;
- hold repeats: the same input and no motion is shown exactly as before, without the option it is not, one 8-bit step of input
  or a motion that is not exactly zero is not a repeat;
- the interface (whole and half), non-finite pixels, and the fixed point of a constant answer.

## What the source guards pin

The three keys are off by default and round-trip (declaration, read, save, `OptiScaler.ini`, `Config.md`); the Vulkan route
never reads them; the constants struct is the cbuffer field by field and a multiple of 256 bytes; the shader's registers are the
root signature's; every hook the rule reads is defined; the pass is registered in the project once; three surfaces; the
renderer builds the object once, only after the gate says run, parks it when the key is off or before the upscaler, deletes it at
shutdown and gives its answer to the light limiter, the down-leg, the unpack and the resolve; the menu controls are hidden on
native Vulkan and under the proxy.

## Not covered

The GPU: half rounding in HLSL, the 3x3 loops, the bilinear history taps. The picture. A game.
