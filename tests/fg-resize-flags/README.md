# FG resize flags

`python3 tests/fg-resize-flags/run.py` compiles the flag transactions sliced verbatim from both FG resize hooks under ASan/UBSan.
It checks the flags that proceed to the resize: accepted waitable creation, retry without the bit, alternating swapchains despite stale global flags, failed GetDesc and non-DLSSG passthrough.
DXGI device work and creation are faked; this proves flag preservation by actual chain description, not native DXGI/provider acceptance or pacing.
