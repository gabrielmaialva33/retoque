# MFG provider capacity and preference

`python3 tests/mfg-provider-capacity/run.py` compiles both production Streamline hooks, CustomOptional, integer serialization and the exact INI read/save statements with real Streamline SDK types and SimpleIni, under ASan/UBSan.
The provider boundary is scripted: limited provider then a patched response at the same function address, a new provider entry point, downgrade, failed/invalid queries and older caller structures. The sent count is clamped while an INI save/load preserves 3 interpolations (4x). Auto/off stay intact.
No DLL is loaded or patched here; real provider/kernel acceptance, native ABI and pacing require the separate Wine/GPU gate.

Menu Default and hotkey off/on after the cap are covered. `--baseline 43d842f8` reruns the same cases against the original hooks using read-only git show, without changing any checkout.

The generation cache cases count provider/patcher calls: 64 stable setters issue one query,
a new generation issues one more, and game-owned GetState replies update the cache without TryApply.
Generation changes during patching/querying are covered. A separate host unit compiles the production
publisher/accessor and checks first/repeated provider identities, completed patch invalidation and wrapper updates.
Only valid replies acknowledge the generation captured before querying; failed/invalid replies can retry.
The publisher unit does not execute the loader or patcher; real DLL mapping/patch completion stays a native gate.

A wrapper-only getter rebind invalidates the cache even without a snippet generation change.
Fake hook entry points are private class members, matching production, so a global cache helper
cannot accidentally access them without receiving the selected getter explicitly.

The `pause-request-*` cases drive the same production setter for PauseWithoutFrameGen's signal
(`State::dlssgGameRequestsFg`): a call Streamline refused is not recorded, also on the own-DLSS-G path; a second
accepted viewport stops any viewport from setting it, while a refused call from one is no viewport; and the menu's
replay (`updateDlssgOptions`, also spliced in) is never taken for the game's request or viewport.
