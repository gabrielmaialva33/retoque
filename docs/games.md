# Games tried with Retoque

Games Retoque has been run in, with the build, the machine and what happened. Each row is one observation on one
day: it says the route reached play on that build without an NR error, not that every setting works, that it
looks right, or that a newer build behaves the same. What each feature is and how far it is proven is in
[capabilities](capabilities.md); the linked records hold the full logs' findings.

Machines: **A** is Arch Linux with Proton (GE-Proton11-11/11-12), RTX 4090, driver 615.78. **B** is native
Windows 10, RTX 3060 12 GB, driver 617.14 then 621.14.

| Game | API | NR route | Machine | Date, build | What happened | Record |
|---|---|---|---|---|---|---|
| Cyberpunk 2077 | D3D12 | after Ray Reconstruction | A | 2026-11-10, `1eb4b94b` | reached play; 1,481 model evaluations, 0 failed; DLSS-G 3x with the MFG unlock; about 27 real fps at DLAA | [field checks](superpowers/reviews/2026-11-10-field-checks.md#cyberpunk-2077-on-proton-rtx-4090) |
| Cyberpunk 2077 | D3D12 | after Ray Reconstruction | A | 2026-10-31, `2da82e81` | built-in benchmark about 20 times; 0 OptiScaler errors | [game smoke](superpowers/reviews/2026-10-31-agent-driven-game-smoke.md#results) |
| Dying Light: The Beast | D3D12 | with sm86 DLSS-G 4x | B | 2026-11-10 | 10 min of play; frame generation stuttered with ~27 real fps (lower the DLSS mode) | [field checks](superpowers/reviews/2026-11-10-field-checks.md#dying-light-the-beast-on-native-windows-rtx-3060) |
| Dying Light (2015) | D3D11 | present, on the D3D11 bridge, with LSFG x2 | A and B | 2026-11-04, `690916b1` | about 2 hours played on each machine | [field runs](superpowers/reviews/2026-11-04-dying-light-lsfg-field-runs.md) |
| DOOM Eternal | Vulkan | after the game's DLSS (the Vulkan present host in menus) | A | 2026-11-05, `6b44efc6` | three sessions, no error; NR before the upscaler (`Stage=1`) was refused, not run | [Vulkan runs](superpowers/reviews/2026-11-05-doom-eternal-vulkan-runs.md) |
| The Witcher 3 (DX12) | D3D12 | after Ray Reconstruction | A | 2026-11-01, `175bc188` | save continued, walked in Novigrad; 1,093 evaluations, 0 failed | [game smoke](superpowers/reviews/2026-10-31-agent-driven-game-smoke.md#2026-11-01-build-175bc188-the-d3d11-routes-and-rrworkingscaleauto) |
| Divinity: Original Sin 2 | D3D11 | present, on the D3D11 bridge, motion synthesized | A | 2026-11-01, `175bc188` | save continued; the model ran on every frame handed to it | [game smoke](superpowers/reviews/2026-10-31-agent-driven-game-smoke.md#2026-11-01-build-175bc188-the-d3d11-routes-and-rrworkingscaleauto) |
| Sons Of The Forest | D3D11 | after the game's native DLSS, with its own guides | A | 2026-11-01, `175bc188` | save loaded, walked at night; every evaluation carried the game's depth and motion | [game smoke](superpowers/reviews/2026-10-31-agent-driven-game-smoke.md#2026-11-01-build-175bc188-the-d3d11-routes-and-rrworkingscaleauto) |
| Starfield | D3D12 | after upscale (DLSS Quality), FG 2x | A | 2026-10-31, `2da82e81` | save continued, walked in Cydonia; 0 OptiScaler errors | [game smoke](superpowers/reviews/2026-10-31-agent-driven-game-smoke.md#results) |

To add a game: play it on a known build, read its OptiScaler log, write a dated record under
[reviews](superpowers/reviews/) with the build, machine, route and what the log shows, then add one row here.
