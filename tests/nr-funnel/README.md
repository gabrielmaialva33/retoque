# NR engagement funnel

```bash
python3 tests/nr-funnel/run.py
```

The engagement funnel ([engagement-funnel.md](../../docs/dlssnr/design/engagement-funnel.md)). The runner compiles
`retoque/dlssnr/DlssNr_Funnel.h` as it is (standard library only) with g++ C++20, ASan/UBSan, `-pthread` and
`-Werror`, runs `cases.cpp`, and checks the production route by source.

## What this covers

- **Balance.** Every evaluate that enters a ladder ends once: ran the pass (with whether the model was called) or one
  named decline; `eligible - ran - declined` is zero. An evaluation that ends with no outcome is left unaccounted, not
  hidden in a bucket, and the first outcome named wins.
- **Concurrency.** Four writer threads and a reader: the reader never sees more outcomes than entries (unaccounted is
  never negative), and the ladder balances once the writers stop.
- **Crash-handler safety.** The ladder is `constinit`-able; the header holds no lock and allocates nothing.
- **Stage marks.** Entering, recording and leaving the route are marked with the thread, as one word.
- **Names.** Every decline has a unique `[a-z0-9_]` tag and a unique text; every route a name and a tag.
- **Source guards.** In `EvaluateAfterUpscale` the funnel scope opens after the NR-off return and before any other
  way out; every later `return;` sits in a block that names exactly one decline; the end counts Dispatch's result
  (ran, or "recorded nothing" with the pass's own reason); the stage is marked just before Dispatch; every decline in
  the enum is used by the route.

## What this does not cover

The counters in a running game, the panel's text (`nr-diagnostic-report` holds the Funnel section's format), and the
routes not counted yet: the D3D12 and D3D11-bridge present hosts and the Vulkan routes.
