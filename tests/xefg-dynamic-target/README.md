# XeFG Dynamic MFG target under V-Sync

`python3 tests/xefg-dynamic-target/run.py` slices two pieces of production code and compiles them with the real
`XeFG_DynamicCount.h` and `XeFG_DynamicCap.h` under ASan/UBSan:

- `XeFG_DynamicMFG::TargetFps` (`OptiScaler/framegen/xefg/XeFG_DynamicMFG.cpp`, where upstream `c2876a76` moved
  Dynamic MFG out of `XeFG_Dx12`), which hands `FramerateTargetDMFG`,
  `FramerateLimit`, the refresh rate and `State::fgPresentVsync` to `XeFG_DynamicTargetFps`. Cases at 60, 120 and
  165 Hz with V-Sync on and off: auto, a target above, below and at the refresh rate, an FPS cap above and below it,
  both together, and an unknown refresh rate (0), which holds nothing.
- `FGHooks::FGPresent` from its `ForceVsync` block to where it records `State::fgPresentVsync`
  (`OptiScaler/hooks/FG_Hooks.cpp`): the final sync interval, the game's own or `ForceVsync`'s, is what the
  target reads as V-Sync, and a `DXGI_PRESENT_TEST` call leaves it alone.
- Both Dynamic MFG controllers on a simulated game at 120 Hz with V-Sync and a target of 180. The controls show
  each climbing to 6X when aimed at 180; on the target the policy gives they settle at 4X, the fewest generated
  frames that fill the refresh rate.

What it does not prove: the simulation's premise, that XeFG drops the generated frames that don't fit the refresh
instead of holding the game, is janblade's observation (Witcher 3, 120 Hz) and not measured here. No XeFG, DXGI,
display or limiter runs; how the limiter at the refresh rate ("Use target as cap") paces against V-Sync, and what
a VRR display does, are not covered.
