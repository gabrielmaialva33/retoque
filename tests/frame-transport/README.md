# Frame transport protocol core

`python3 tests/frame-transport/run.py` compiles the actual experimental
[core](../../retoque/frame_transport/Protocol.h) with C++20, warnings as errors and ASan/UBSan.
It needs no Windows headers, IPC, GPU, Wine or model. Build output stays under ignored `artifacts/`.
Registered as `frame-transport`, host tier, transport domain.

Cases cover exact little-endian wire fields, every truncated hello prefix, malformed lengths and kinds,
reserved bytes, all stream splits, coalesced packets, partial EOF, sticky framing errors, 4096 deterministic
malformed packets, capability intersection/required features, one/two-slot limits, ordered consumption,
monotonic fence/serial/generation identity, one model recording, repeated return/release, resize while busy,
old generations, device-loss sentinel and producer/consumer death or timeout in all three occupied states.

Ten isolated header mutations must compile and each fail its own exact diagnostic: wrong version,
truncated packet, oversized packet, double return, wrong generation, peer death freeing a lease, queue
saturation, ignored completion, device loss without a latch, and missing required capability. A crash,
compiler error, unrelated failed assertion or surviving mutant does not count as rejection.
A source/build reference guard holds the unwired boundary.

The tests prove decisions over synthetic observations, **not** that an observation came from a GPU.
There is no handle registration/import, authenticated channel, clock/deadline scheduler, render hook,
presentation adapter or renderer integration here. No Windows/Proton compatibility, latency, performance,
image quality or security isolation claim follows. See the [design](../../docs/dlssnr/design/frame-transport.md).
