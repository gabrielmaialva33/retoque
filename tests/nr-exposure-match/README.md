# Exposure scan match (host)

```bash
python3 tests/nr-exposure-match/run.py
```

`retoque/dlssnr/DlssNr_ExposureMatch.h` under ASan/UBSan on synthetic traces (a lagged exposure, divisor and stop
forms, a timer, frozen and paused values, partial and mostly-clamped values, the scene's luminance), janblade's
ported scenes, this fork's meter, offered value, easing and recorder, and the trimmed recordings in `data/`
(Cyberpunk 2077, Doom Eternal, NBA 2K27; GPL-3.0, from janblade/OptiScaler-F5-DLSSNR-Multipass @ 61c97d64). Then
source guards on the wiring. Design: `docs/dlssnr/design/exposure-scan-match.md`. Nothing here runs a GPU or a game.
