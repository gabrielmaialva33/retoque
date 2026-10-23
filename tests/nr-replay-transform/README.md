# Metamorphic replay captures (host)

```bash
python3 tests/nr-replay-transform/run.py
```

Unit tests of `tools/replay/transform.py` (see `tools/replay/README.md`, "Metamorphic captures") on a small synthetic
v1 capture shaped like the loopback corpus. Stdlib; NumPy when present, held equal to the scalar path.

- **pad**: every depth and motion texel of the valid region is the source's, at its base, and every margin texel is
  the sentinel; colour is unchanged; the render subrect, bases and MV scale are set; vectors x 2, x -1 and x 1/4 give
  stored x scale equal to the source bit for bit; x 3 and an inexact subnormal are refused; MVLowRes is toggled at
  1:1. The seam's own `run_replay.load_capture` accepts the result and `write_plan` writes its bases and scale.
- **colour8**: RGBA8 and BGRA8 carry the same colour once the bytes are reordered, alpha varies, R sits far from B,
  guides and resets are the same; `load_capture` accepts both. The alternating capture follows its schedule, its
  reset falls on the first scored swap frame and on the same frame of the all-RGBA reference. Whether the seam takes
  it is printed as a NOTE (today: refused, "the shape changes within the capture").
- **compare**: BGRA outputs are read back as RGBA, so a correctly reordered run is identical; RGBA bytes labelled
  BGRA (a missed reorder) are caught on every frame, and so is a changed decision.

No GPU, DLL, model or Proton: the replay itself is `tools/replay/metamorphic_seam.sh`, run under the GPU lease.
