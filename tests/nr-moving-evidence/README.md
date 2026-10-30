# Moving-scene evidence checks

Run `python3 tests/nr-moving-evidence/run.py` with Python's standard library.

This runs the independent analyzer's tests from `dlssnr-loopback/test_moving.py`:
known projected motion must agree with the depth/camera oracle, zero or nonfinite
motion must fail, disabled processing must fail, repeated-output noise must not
hide the processing effect, and the timing percentiles use nearest rank.

Host fakes compile the actual sweep coverage consumers and `SweepDrain`, checking that failure exits
without unwinding GPU-owned objects. They exercise per-step coverage and fence decisions, including signal,
event, timeout, wait failure, incomplete completion and device removal. Compiler-entrypoint tests
exercise lease contention, owned fake processes, unrelated same-name survival, timeout, interruption,
lock release, failure propagation and retained logs. These do not execute Wine or GPU work.

The actual analyzer is shared with the real moving-scene GPU experiment. These
checks exercise its decisions on known inputs without Wine, NGX or a GPU; they
cannot establish rendering or model correctness.

The D3D12 present-guide and native D3D11 bridge probe validators also run here. Their
synthetic host cases reject missing/wrong DLLs, stale logs, zero guide coverage, wrong
consumer and incomplete or failed runtime reports. These checks do not execute a DLL,
Wine, NGX or a GPU; a real probe run is separately required.
