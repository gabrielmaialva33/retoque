# Proton: the game's DLSS-G instead of the driver's Wine copy

Run `python3 tests/proton-dlssg-provider/run.py` on the host with g++.

Under Proton the NVIDIA Linux driver's Wine build ships its own `nvngx_dlssg.dll` (`/usr/lib/nvidia/wine`) and its
NGX core loads it as the DLSS-G provider even when the game ships a newer one. The Ada MFG unlock did not recognise
driver 620.73.04's 310.2.1, so frame generation stayed at 2x in The Witcher 3 on an RTX 4090 while the game's 310.9.16,
patched, sat unused. With `[DLSSG] AdaMfgUnlock` on, under Wine, `AdaPreferGameProvider` (default true) answers that
load with the game's own newer file. `retoque/framegen/dlssg/ProtonDlssgProvider.h` decides it.

The runner compiles the rule under ASan/UBSan against real paths (pressure-vessel `z:\run\host\...`, plain
`z:\usr\...`, the game's copy, other features in the same folder, a bare name, an unrelated `wine` folder) and every
input of the decision; seven rule mutants must fail. The load hook's wiring (full path rather than the bare name,
the three gates, the redirect before the load, the unlock after it) and the config key's declaration, read, save and
shipped INI entry are read off the source.

It does not run Wine or NGX: that NGX accepts the game's file in place of its own copy is shown by a game run.
