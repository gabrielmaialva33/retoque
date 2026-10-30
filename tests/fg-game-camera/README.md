# Streamline game camera forwarding

Run `python3 tests/fg-game-camera/run.py` on the host with g++.

The suite compiles the real Streamline `sl::Constants` definition, the owned camera cache,
the production camera-construction/`SetConstants` output statements, input-frame mapping,
`StartNewFrame`/`GetDispatchIndex`/counter methods and cache-clearing lifecycle prefixes under
ASan/UBSan. It covers high external frame IDs, present resetting the logical counter, queued
versus resource-driven dispatch decisions with different frame-ahead allowances,
delayed input, exact dispatch selection, invalid replacements, missing constants at ring reuse,
deactivation/resource release, Unreal fallback, invalid/incomplete matrix groups, optional
lens/pose validation and concurrent input/present access.

The DirectX projection constructor and SDK callback are fakes. This verifies CPU forwarding
and lifetime decisions, not camera correctness in a particular game or GPU/image quality.

The suite also compiles the real `Nvngx_FG` D3D12/Vulkan create and release methods against a
fake provider and an instrumented mutex. It checks failed creation, no published handle or
leak, provider absence, invalid output, provider failure and unlock-before-destruction. Camera
fallback checks cover nonzero position and finite projection arguments for infinite far depth
(including reverse Z); the projection itself remains a host fake. These do not establish
concurrent handle lifetime safety or game/GPU behavior.

The fixes were informed by [HelixSR-Scaler](https://github.com/lonewolf0622/HelixSR-Scaler)
commits [5e1b0492d0](https://github.com/lonewolf0622/HelixSR-Scaler/commit/5e1b0492d050ded74cac4780555aefc9eb92e65f)
and [b8f184987c](https://github.com/lonewolf0622/HelixSR-Scaler/commit/b8f184987cc5f16fc283919798ae9dd6c91e5f64)
(GPL-3.0); the existing game-camera override and finite-depth behavior remain in place.
