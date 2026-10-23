# FG DLSS-G back buffer index

`python3 tests/fg-dlssg-backbuffer-index/run.py` slices FGPresent's DLSS-G block verbatim from
`OptiScaler/hooks/FG_Hooks.cpp` (from `if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG)`
to the next block) and compiles it against Streamline and DXGI fakes under ASan/UBSan.

DLSS-G generates nothing in a frame without a `GetCurrentBackBufferIndex` call on its swapchain
(`sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled`, `external/streamline/sl_dlss_g.h`). A game that
tracks the index itself never makes that call, and OptiScaler made it only on the path where it sends its own
Reflex markers. The matrix (present or test present, FG active or not, the DLSS-G, FSR-FG and XeFG outputs, the
game sending markers, `[DLSSG] UseGamesReflexMarkers`, PCL loaded, the frame token made or refused: 192 cases)
checks the call is made exactly once on every DLSS-G present and never otherwise, ahead of the frame token, and
that the token and `ePresentStart` stay on their old condition.

Before the fix, 10 cases failed: every DLSS-G present where the game's markers are used, or PCL is not loaded.
The fakes stand in for Streamline and DXGI; this proves the call is made, not that DLSS-G then generates frames.
