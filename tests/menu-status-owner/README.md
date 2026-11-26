# Menu owner and NR Status

Run `python3 tests/menu-status-owner/run.py`.

Compiles the production pure owner/status headers and the extracted production `OptiInputWndProc` against
minimal Win32 fakes, plus the actual Status collector/drawer with Config/NR/ImGui fakes, with g++ C++20, ASan/UBSan and warnings as errors. Covers alternating windows for six
seconds, a destroyed owner, exact 1 s timeout, ignored null presents, non-refreshing secondary presents,
clock rollback, last-message forwarding without the input mutex, lost-chain erasure, unknown and reused HWNDs.

The Status matrix covers off with stale model/failure data, failure, waiting, D3D12/native Vulkan,
hidden edit, requested pass/cadence wording, resolved regular/RR scales and model basename privacy.
The collector fixture verifies the NR-off early return, table-not-visible early return, source values,
driver refusal, D3D11 no-pass hook suppression and native scene override. Source guards keep ownership before device queries under the overlay mutex, retain the upstream foreign-chain
refusal, bind Status to the diagnostic getters, reject costly/mutating calls, and register the read-only card.

No MSVC/DLL, Win32, ImGui, Vulkan, GPU or game execution. Fakes do not qualify OS subclass ordering or layout.
