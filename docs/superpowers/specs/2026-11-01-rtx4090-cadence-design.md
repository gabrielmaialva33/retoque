# RTX 4090 cadence work elimination

Date: 2026-11-01. Baseline: `73fad086`. Status: design for an isolated, measured optimization.

## Intent and authorization

The user asked to optimize Retoque as far as possible on the RTX 4090. This round targets redundant GPU work at the current image quality. The controller's working choice is to preserve full NR working scale (1.0), the model, provider, frame-generation settings and image mathematics. The user has authorized implementation and reversible validation; commit, merge and push are not included.

## Evidence

The current Cyberpunk after-RR record shows about 0.445 ms outside the model with adaptive cadence configured at 4, versus 0.223 ms at Cadence=1. Adaptation usually chooses interval 1. The 2026-10-31 fade optimization helped fixed cadence, while the adaptive case stayed near 0.444 ms. These are historical observations, not this candidate's results. A new baseline is being recorded independently.

Record currently writes the full residual, proxy and depth history, then builds the coarse residual by reading the full residual again. A subsequent model frame can replace all of that coarse image before a carry/fade consumes it. The adaptive probe calls CadenceCarryEdit but observes only its acceptance field; acceptance itself depends on the chain, depth and proxy, not on the residual or coarse fill. DXC may already eliminate unused fill work, so factoring the probe is a dependency clarification, not a claimed GPU speedup by itself.

## Approaches considered

1. Reduce working scale or increase temporal reuse. This changes image detail/stability and is outside this quality-preserving round.
2. Fuse or parallelize record/coarse. This adds a shader algorithm and potentially changes reduction rounding and synchronization. Keep it as a later measured alternative.
3. Preserve the history and acceptance arithmetic, but calculate the coarse residual only at its first real consumer. Selected because it removes a full-image reduction without changing its inputs or summation order.

## Behavior and interfaces

- Extract the existing nine-tap acceptance and clamped reprojection coordinates into a shared pure rule: CadenceAcceptance (weight, qx, qy), returned by CadenceAccept(px, py, acc, P). Preserve the existing arithmetic, order, edge behavior and non-finite motion handling.
- CadenceCarryEdit uses that rule and retains its edit/fill/UI mathematics and stopWhenRejected behavior. The adaptive probe uses the same acceptance rule directly and no longer binds/reads residual or residualLow.
- Record retains the original full residual/proxy/depth writes and transitions. It no longer computes coarse.
- Compute the unchanged coarse reduction at the beginning of the first Carry after a model Record, using the existing InChainStart scheduling fact. Later carries reuse it. Fade is reached only after carried frames and therefore sees the matching coarse image before Record replaces the history.
- Prefer this existing scheduler fact over a new CPU dirty/ready flag. Any discovered call path that invalidates the fact must be resolved before implementation; do not paper over it with untracked readiness.
- No change to model evaluation schedule, adaptive thresholds, cadence counts, resource allocation/retirement, cbuffer layout, config keys, NR-disabled behavior, or other APIs.

## Correctness boundaries

The calculation moves in time but uses the same stored residual and dimensions; frame-local depth/motion must not affect coarse construction. Keep write-to-read transitions and ordering. The first carry after a reset, gap, signature/size change, failed model or refusal must never consume a previous history's coarse image. A fade must read the previous model's history before the new Record. No new fences, waits or CPU readbacks. Existing submission/generation ownership and descriptor ring limits remain unchanged.

## Acceptance

1. A regression must fail on the old scheduling before the change: a run of model/probe frames at adaptive interval 1 makes no coarse dispatch; a first real carry makes one before any coarse read; later carries/fade reuse the same history; a new Record restarts that relation.
2. Exercise production acceptance over still scenes, borders, partial/full rejection, occlusion, moving surfaces, UI and non-finite motion. The independent previous behavior is the reference; acceptance-only evaluation performs zero residual/coarse reads. Existing image-rule tests remain green.
3. Test actual production orchestration through a bounded fake command/resource sink or the existing runtime fixture. Do not make source-string assertions the only evidence for dispatch order. Preserve existing guards rather than replacing behavioral proof with them.
4. Targeted cadence suites and full host tier pass; shader bytecode is regenerated with the existing recipe; Release DLL and forwarder build successfully. No build runs while another owner holds the compiler prefix.
5. Baseline/candidate replay compares the same recorded inputs and checks output equality or reports any difference. Cover fixed cadence, adaptive interval 1 and recovery to carries, smoothing on/off, reset and odd-sized edges where fixtures allow.
6. Interleaved real-game measurements retain exact DLL/model/config identity. Separate model and carried samples, record mean/p99 rendered frame time and temperature/clocks. A pass-time saving is not automatically an FPS gain.
7. Reject or revise the candidate if output changes, a route regresses or the first-carry work causes an unacceptable frame-time tail. Native Windows coverage remains separately reported.

## Operational boundaries

Work only in the isolated perf worktree. One owner at a time for GPU experiments; one owner for compiler-prefix work. Preserve user settings and restore INIs byte-for-byte after experiments. Never overwrite a DLL mapped by a running game. Do not change VRR, clocks, power limits, drivers, model binaries or global Steam settings. Keep raw captures, machine paths and model data out of Git. Publication/integration are separate from the local optimization work.
