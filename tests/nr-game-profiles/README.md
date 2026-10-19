# NR game profiles (host)

```bash
python3 tests/nr-game-profiles/run.py
```

`retoque/dlssnr/DlssNr_GameProfiles.h` holds the keys some games need before NR runs in them at all
(`docs/dlssnr/design/game-profiles.md`). It is header-only and free of `<windows.h>`, so `cases.cpp` compiles it as
shipped with g++ C++20, ASan and UBSan:

- **Match:** exe names compare without case; a missing extension, a near miss, Divinity's other exe, the Witcher 3
  DX12 exe and an unrelated game match nothing.
- **Table:** every profile has a name, exes stored lowercase and ending in `.exe`, one profile per exe, evidence,
  something to apply, a HookMethod of 0..2 and an upscaler code the converter knows.
- **Apply rules,** against a fake option with the `CustomOptional` surface the production code calls
  (`has_value`, `set_volatile_value`): with nothing set every key goes in volatile and keys the profile does not name
  stay untouched; a value from the user's ini wins key by key and is reported as left to the ini.
- **Proton's nvapi64.dll:** only `WINEDLLPATH` entries ending in `/wine`, a trailing slash, the file having to exist,
  relative and empty entries ignored, first match wins, the nvml entry Proton prepends skipped.

`run.py` then checks the wiring from source: `Apply` tests `[DlssNr] Enabled` and `GameProfiles` before it touches
anything (default-identical with NR off), `NvapiPath` is only ever set volatile (an assignment would be written back
by a menu save), `dllmain.cpp` calls `Apply` exactly once, right after upstream's `ApplyGameConfigs`, and
`GameProfiles` exists at all four config points.

Not covered: the real `Config`, a real `WINEDLLPATH` under Proton, or any game. Those are the in-game checks listed in
the design note.
