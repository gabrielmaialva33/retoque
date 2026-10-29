# Half-rate look experiment

See [design and measured decision](../../../docs/dlssnr/design/look-model-half-rate.md).
These scripts are an offline experiment, not a renderer feature or a quality gate.

`look_model.py` needs NumPy and existing diagnostic replay runs. It validates manifest
identity and raw plane hashes, simulates even-ID network / odd-ID prediction, and
calls the existing NumPy replay metrics. Resets force the network oracle. It never
loads a DLL, opens a game or writes input captures. A fresh output directory is
required. The `hold` baseline is simple previous-output reuse, not production
motion/depth cadence. Reports contain whole-scored and predicted-only aggregates,
per-frame metrics, training reconstruction error and a deliberately unsafe cut probe.

```sh
OPENBLAS_NUM_THREADS=1 uv run --no-project --with numpy python \
  -m tools.replay.experiments.look_model --self-test

systemd-run --user --scope -p MemoryHigh=8G -p MemoryMax=10G \
  -p MemorySwapMax=2G -p OOMPolicy=kill -- \
  env OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 \
  uv run --no-project --with numpy python -m tools.replay.experiments.look_model \
  --capture "$CAPTURE" --runs "$DIAGNOSTICS/replay-run/runs" --out "$FRESH_RESULT"
```

The measurement supports v1 raw float captures and exactly three diagnostic run
names/settings: `proxy` (Cadence=1, DebugView=1), `every-a` and `every-b`
(Cadence=1, DebugView=2). All have Enabled=true. Use the standard replay defaults
WhitePointScale=1, WorkingScale=1, transformed HDR input and Transfer=1. The
experiment's recorded corpus uses fixed unit exposure; changing these assumptions
requires a new domain review. RGB values are reconstructed with the existing
saturating sRGB transfer. The result measures decoded model colour, not the final
composed game image or unbounded raw network output. The full-rate oracle's hidden
state is retained, so this does not simulate the network's history after real skips.

If diagnostic inputs are missing, `look_diagnostics.py` can replay a **supplied**
capture using the existing production debug views. This is a separate GPU operation;
the NumPy prototype itself remains CPU-only. It uses `run_moving.lease`, including
the shared prefix lock and active-game refusal; do not wrap it in another lock.
It creates an isolated Proton prefix/output directory and never compiles or captures
a new scene. Supply the retained harness binary and a DLL with a forwarder sibling:

```sh
python3 -m tools.replay.experiments.look_diagnostics \
  --capture "$CAPTURE" --dll "$BINARY_PAIR/OptiScaler.dll" \
  --harness "$HARNESS" --out "$FRESH_DIAGNOSTICS" > "$DIAGNOSTIC_LOG" 2>&1
```

The usual replay runner supplies the local model kit, DLSS snippet and driver core.
Their real identities are recorded in the logs/manifests. The wrapper records the
DLL/forwarder/harness hashes too. `producer.commit` identifies orchestration source,
not the reused binary's build; check the binary hash and logged build ID.
No calibration is generated: A/A equality is descriptive, never a fidelity PASS.
The compact committed result file contains numerical evidence only; pixels, models,
prefixes, full logs and frame records remain in ignored local artifacts.
