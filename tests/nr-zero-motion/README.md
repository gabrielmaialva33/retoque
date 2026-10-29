# nr-zero-motion

Host suite for zero-motion detection (`docs/dlssnr/design/zero-motion-detection.md`): the game's motion vectors
called dead after two readings of an all-zero field while the scene's depth moved, and `ZeroGuideReset` then dropping
the model's history on them as it does on zero guides.

Run: `python3 tests/nr-zero-motion/run.py` (needs `g++` with C++20, ASan and UBSan).

`test.cpp` compiles the production header `retoque/dlssnr/DlssNr_MotionProbe.h`, the code the renderer runs on every
probe readback, and drives it with synthetic 32x32 grids shaped as `dlssnr_motion_probe.hlsl` writes them:

- the grid coordinates (a cell's centre, a motion tap) inside every extent, and taps that never repeat once the
  extent allows;
- the read-out: a cell that did not read the full lattice, or a NaN count, makes the probe invalid, and an invalid
  probe changes nothing;
- a still camera under TAA jitter on a sloped plane: depth moves by less than two pixels of its own slope, so never
  a moving scene, never dead;
- a pan and a walk forward with zero motion: one reading is not a verdict, two are; the sky is left out of the count;
  the verdict holds through more readings and a still one, and one non-zero sample anywhere ends it, once;
- real motion, motion in one cell only, and NaN motion: never dead;
- a third of the frame moving (water with no vectors written) under a still camera: not a moving scene; the
  half-of-what-can-tell threshold at its edge; non-finite depth cannot tell;
- a shape change, a probe too old, and one out of order: not compared; a shape change while dead is said once;
- the reset rule against all sixteen inputs: dead, the game's guides, `ZeroGuideReset` on, and no reset of the game's.

`run.py` also guards the GPU half and its wiring: the constants equal the cbuffer, the registers equal the root
signature, the shader's grid, taps and sampling expressions equal the header's, a NaN vector counts as non-zero,
depth reads are clamped to the region, the bytecode is optional and committed with its `.cso`, the probe is recorded
once in `Dispatch`, only on the game's guides, after they were made readable and before the resample or the
periphery pack read them, with the region the pass computed; the rule is applied once where the frame's reset is read
and nothing after it reads the raw flags; completion goes through the submission tracker with no CPU wait; the pass is
parked through the retired list, freed with it, and freed at shutdown only once its readbacks settled; the session is
file-local, not a new shared global; the files are registered in the project.

What it does not cover: the shader on a device, a real readback, or a game. Those are the in-game checks at the end
of the design note.
