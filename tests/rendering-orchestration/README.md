# Rendering orchestration controls

Run the host controls with:

```sh
python3 tests/rendering-orchestration/run.py
```

These tests exercise the rendering entrypoint's command plan, strict evidence validation, immutable receipt/log snapshots, build identity, atomic reports, and process ownership. They do not compile the production DLL, start Wine/Proton, or use a GPU. Controlled Python child processes stand in for expensive fixtures.

## Entrypoint behavior

`tools/test-rendering.sh` delegates to `tools/test-rendering.py`. The full entrypoint runs strict complete host checks, the optional Release build, FG input checks, moving-scene and replay NR, four required Vulkan cases, then strict DLSS-G provider checks. `--host-only` stops after the host tier; `--no-build` reuses existing production outputs. `--fg-steady-seconds N` explicitly requests FG qualification for integer 5..120 seconds per active phase; omission retains the quick schedule.

Steps run serially. Any required non-PASS result stops the plan and records remaining steps as `NOT_RUN`. Reports retain the actual command, return code, independent fixture status, metadata, receipt/log snapshot hashes and bytes, and production DLL/forwarder hashes before and after each step. A secondary interruption-receipt write error does not erase a known `TIMEOUT` or `ABORTED` supervision outcome.

Loopback's moving/replay entrypoints already own the shared compiler-prefix lease. The orchestrator does not double lock them. Vulkan receives an outer `flock -n -o` lease; the close-on-command behavior keeps descendants from retaining its lock descriptor. Build, FG input, and provider commands retain their existing leases.

## Owned nested cleanup

The orchestrator reuses `tests/run_all.py` process supervision. The loopback entrypoint uses the existing `result_run` interrupt guard so SIGTERM becomes an exception and reaches `run_moving.execute` cleanup. Its interruption-only receipt is `artifacts/interruption-result.json`; successful mode-specific legacy reports remain authoritative.

Loopback delegates detached Proton-group cleanup to the same `_stop_child` helper with a 2 second grace, shorter than the outer runner's default 4 seconds. The helper retains its own leader unreaped through TERM, grace, and final KILL, then waits. A successfully reaped leader never authorizes further numeric process-group signals. Default SIGCHLD is required before launching supervised loopback work. Cleanup targets owned groups and does not reset global Wine state.

The native nested tests use the actual staged loopback entrypoint, actual inner execute, and actual outer suite runner. A detached child and its descendant both ignore TERM; a readiness handshake confirms the descendant installed its handler. Timeout and interruption must remove these owned children while leaving an unrelated sibling alive. These tests require native Linux process identities, verified by `/proc/self/stat` matching `os.getpid()`. They skip in environments with virtualized process IDs; a native-host run is then required before claiming nested timeout containment. Portable tests still verify actual subprocess SIGTERM receipt/exit behavior, default-SIGCHLD refusal, no reap before final group kill, the shorter inner grace, and no signal after reap.

## Evidence scope

A host-only PASS means host checks only and reports `runtime_coverage=false`. A full PASS means all required scenario/runtime/seam gates passed against the selected binary hashes. It does not approve a hardware family, image quality, latency, display scanout, or native Windows behavior.

Moving-scene metadata checks include real finite positive timing samples with consistent percentiles, geometric guide coverage, processing/repeat bounds, temporal sample identities, measured frame hashes, and staged build provenance. Replay requires its default functional seam gates; its named informational pixel-repeat metric remains informational. Original optional pixel-judge verdicts remain visible in a separate quality scope.

Raw Vulkan validation/VUID/SYNC-HAZARD messages remain visible even when a fixture attributes its required scenario PASS. Reports retain full log snapshots, total matching-line counts, and bounded excerpts. This provides evidence for review without issuing a universal validation-clean verdict.
