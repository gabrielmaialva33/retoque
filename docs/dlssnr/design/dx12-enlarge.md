# D3D12 renderer extraction: enlargement (step 5)

Move PrivateSrApi, PublishEnlarge, PlanEnlarge, FailEnlarge, BuildEnlarger and
EnlargeEdit to DlssNr_Dx12_Enlarge.cpp. Keep NrEnlarger complete in Internal.h
at every deletion site, including TickNrRetired and Shutdown. Move CoreGone and
the destructor out of line to the enlargement unit; preserve their bodies and
member declaration/initialization order. EnlargeFrame also belongs in Internal.h
because Dispatch constructs it. Shared storage (g_enlarge included), retirement,
retry/status, Dispatch and routes stay in core, in their original order.

Release order is unchanged: feature Release (or Abandon during process exit or
NGX core shutdown) precedes carrier/enlarged/exposure/depth/motion releases.
Retirement first finishes modifying the list, then deletes the collected bundles.
No ownership, locking, creation admission, fallback, shader, parameter or clock
behavior changes. Register the compile unit only in the project/filter fork tails.

nr-enlarge must explicitly assemble types from Internal.h, model result names
from Model.cpp, enlargement definitions from Enlarge.cpp and storage/retirement/
retry/status from core. Use unique definition extraction, not a concatenated slice
across namespace or adjacent-function boundaries. Preserve its real private-SR
adapter and fake-driver/state/release-order tests. Extend the ownership manifest.

Gates: memory-capped host with NumPy, clang-format 22, diff check, locked Release,
and 17 frozen-baseline replay variants (calibration and judge off). Build and replay
run as separate commands. Count scale-half-t2 only after confirming successful
private DLSS evaluation and Transfer 2 selection in its log, with matching run
provenance; a configured key alone or permanent fallback is not coverage.

Replay remains after-upscale synthetic-scene evidence. Host tests cover core
shutdown/abandon, failure and release ordering through fakes; neither those nor
Proton establish live game teardown, native Windows debug-layer correctness or
pre-upscale/present route behavior.
