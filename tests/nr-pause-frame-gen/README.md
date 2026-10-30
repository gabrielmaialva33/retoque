# Pause NR without frame generation, contract

Run `python3 tests/nr-pause-frame-gen/run.py`. A source contract, plus `viewports.cpp` under ASan/UBSan.

Checks `[DlssNr] PauseWithoutFrameGen` (`docs/dlssnr/design/pause-without-frame-gen.md`): the four-point
config, off by default; the signal is the game's own DLSS-G only; the pause arms only after it was seen on,
never while our menu is open, after 30 entries off, and resumes at once; and the check sits in `Dispatch`
before any tracking or recording, so a paused frame goes out exactly as the game drew it.

Since 2026-10-30 (after kkyleeb21's D18 0.4.1, 3f61eb29): the hook publishes the game's request only once Streamline
accepted the call (`eOk`), never for its own menu replay (`updateDlssgOptions`), and only from the one viewport that
drives DLSS-G; the rule stands down once a second viewport was accepted. `viewports.cpp` runs the real
`retoque/hooks/Streamline_DlssgRequest.h`: one owner, a second viewport sticky, the ends of the handle range, nine
threads on one viewport and two racing. The real hook's behaviour on refused calls, two viewports and the replay is in
`tests/mfg-provider-capacity` (`pause-request-*`).

It does NOT run a game or a GPU; whether a given title turns DLSS-G off in its menus is an in-game check.
