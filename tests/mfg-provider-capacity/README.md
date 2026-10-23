# MFG provider capacity and preference

`python3 tests/mfg-provider-capacity/run.py` compiles both production Streamline hooks, CustomOptional, integer serialization and the exact INI read/save statements with real Streamline SDK types and SimpleIni, under ASan/UBSan.
The provider boundary is scripted: limited provider then a patched response at the same function address, a new provider entry point, downgrade, failed/invalid queries and older caller structures. The sent count is clamped while an INI save/load preserves 3 interpolations (4x). Auto/off stay intact.
No DLL is loaded or patched here; real provider/kernel acceptance, native ABI and pacing require the separate Wine/GPU gate.

Menu Default and hotkey off/on after the cap are covered. `--baseline 43d842f8` reruns the same cases against the original hooks using read-only git show, without changing any checkout.
