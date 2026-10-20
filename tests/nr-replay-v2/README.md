# Replay v2 (host)

`python3 tests/nr-replay-v2/run.py`: native DXGI decoders, metadata refusals,
render/output guide geometry, jitter, and exact v1/v2 metric/verdict conformance.
NumPy comparisons skip when absent; use `uv run --with numpy` to exercise both.
No GPU, DLL or game execution. Real-baseline conformance is separate local evidence.
