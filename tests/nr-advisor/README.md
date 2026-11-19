# NR cost advisor host contracts

`python3 tests/run_all.py --only nr-advisor --jobs 1 --strict`

Compiles the production pure core with ASan/UBSan. Exercises candidate order, settle and sample counts,
duplicate/stale/unconfirmed/malformed timing, statistics, target-FPS recommendation, refusals and restoration
on all abort reasons. Also compiles the actual menu against event fakes, the production adapter, and the production
GpuTiming collector with its existing synthetic device/submission fixture: no advisor sample before
fence completion, no duplicate collection, and old run tokens rejected. Small static source checks
cover the local override and close/resize hooks; they do not compile the full D3D12 Dispatch.

No GPU calls, game, shader, visual quality or performance claim. See
[design](../../docs/dlssnr/design/nr-advisor.md) for required game acceptance.

Round 2 also covers RR-only Apply routing, no drawing on unavailable backends/off, explicit existing-key timing
preparation, the shared Config admission policy and module heartbeat, evaluation-ordinal gaps/failed-scope exits,
model/carried counts, mean and slow-tail statistics, and bimodal cadence windows rejected despite a low median.
The unchanged runtime cadence policy still refuses FG without CadenceWithFrameGen; the advisor does not hide
scale trials in that case. See the design for the Streamline menu interlock's measurement limitation.
