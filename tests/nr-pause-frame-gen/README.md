# Pause NR without frame generation, contract

Run `python3 tests/nr-pause-frame-gen/run.py`. Pure Python.

Checks `[DlssNr] PauseWithoutFrameGen` (`docs/dlssnr/design/pause-without-frame-gen.md`): the four-point
config, off by default; the signal is the game's own DLSS-G only; the pause arms only after it was seen on,
never while our menu is open, after 30 entries off, and resumes at once; and the check sits in `Dispatch`
before any tracking or recording, so a paused frame goes out exactly as the game drew it.

It does NOT run a game or a GPU; whether a given title turns DLSS-G off in its menus is an in-game check.
