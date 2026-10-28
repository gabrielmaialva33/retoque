# Vulkan readback, NR scaler, and sustained FG qualification

Recorded on 2026-10-28. All timestamps in this document are UTC.

**Status:** the final required serial qualification and the final WorkingScale=1.25 GPU regression check are both **PASS**. The manual qualification gate is closed. Earlier startup timeouts remain preserved below. Integration and artifact delivery are recorded separately by the post-execution manifest described at the end of this record.

## Source and artifact identity

- Primary repository: this checkout, on `dlss-neural-rendering`.
- Implementation worktree: `hardening2-20261027` under the local worktree cache.
- Implementation branch: `fix/vulkan-readback-scaler-20261027`.
- Qualified source: `d845b511e1b380ba2cf83d1e8635bbd62502ef16`, clean when the required run started.
- Pre-integration primary check on 2026-10-28: `dlss-neural-rendering`, clean at `ff7dc45b10ee898aead9b02f6587a8cca5c65e69`.
- Before final documentation changes, the implementation worktree was checked clean at `d845b511` on 2026-10-28.
- Subsequent documentation work must not be described as a fresh runtime qualification of changed production code.

| Qualified artifact | SHA-256 |
| --- | --- |
| `x64/out/OptiScaler.dll` | `3f6a2abc1ba497c52b469949ca513af52d716083d4c2d2278d0e4ead2e9311f2` |
| `x64/out/nvngx.dll_dlssnr.dll` | `0ffc8e8902148b10db8555d6ecd76d772ee2343930df32e4aac57e5b2319d011` |

The qualified pair must be promoted byte-for-byte. Rebuilding would require checking the resulting identity before reusing this qualification.

## Implemented changes

1. Vulkan meter readback uses transactional allocation/bind/map construction and a bounded four-slot reservation ring. Recording ownership prevents premature reuse; retirement publication uses release/acquire ordering. Consumption selects a valid retired sample, while invalidation preserves slots still owned by live recordings.
2. NR scaler recordings retain their descriptor leases, immutable aligned constant slices, and generation ownership. All required passes are reserved before the first GPU command; failed admission leaves no partially recorded scaler batch.
3. NR-only scaler shaders now declare storage compatible with RGBA16F targets. The constructor preserves FSR priority, BCUS upsampling, and the existing downfilter selection. Failed replacement leaves rendering disabled while preserving cleanup ownership; non-RGBA16F destinations are rejected before recording effects.
4. Sustained FG qualification accepts `--steady-seconds` from 5 through 120. It retains the original short-run behavior and warmup, then checks chronological samples and complete windows of at least five seconds. An unqualified sustained x2 result is inconclusive and nonzero.
5. The rendering entry point runs host, Release build, FG inputs, moving NR, replay, Vulkan, and provider qualification serially. Fresh run IDs, receipts, binary hashes, retained logs, and owned cleanup make failures inspectable. Later stages remain `NOT_RUN` after an earlier failure.
6. CI preserves artifacts on failure. Native Vulkan result and validation sidecars are retained automatically. Documentation records the actual qualification scope and remaining gaps.
7. The Windows/Linux installer parity fixture now excludes a machine-owned Wine NGX driver with `--no-nvngx`. Production installer behavior and strict receipt/content assertions remain unchanged.

Implementation commits, in order: `5a4c7f16`, `eb7866bf`, `3d125ae4`, `3bd3b712`, `7ad3f9e3619eb22ad2f30ab8d70e87e8fa16f2e2`, and `d845b511e1b380ba2cf83d1e8635bbd62502ef16`.

## Targeted regression verification

- The original embedded scaler modules produced nine genuine format/capability failures in the new structural checks.
- The repaired nine modules passed `spirv-val` for Vulkan 1.0 and the parser checks for image format, bindings, component width, entry point, workgroup dimensions, and exact generated-header/module identity.
- All checked FSR storage writes have four components. The NR modules do not rely on formatless storage-image writes.
- Three new behavioral groups first failed against the previous constructor/dispatch implementation: invalid output admission, failed pipeline replacement, and selected shader bytes.
- After repair, all ten behavioral groups passed under both GCC and Clang, including all sixteen filter/direction selections.
- Upstream Output Scaling and Magnifier shader sources and generated artifacts remain unchanged. The NR BCUS variant preserves RGB filtering math and uses an explicit opaque-alpha store.
- Pinned clang-format 22 checks, `git diff --check`, and independent review passed.
- Generated header SHA-256: `099902452d6c8df6c019b4fbfcee8f7380c787eddce2c2452e7b1cf14f2c7f1c`.
- DXC version: `1.10(5373-c4d8f4f9)(1.10.2605.37)`; executable SHA-256: `f315447a5fda680162d0ee374ed8ac20ce1e2f922b008da264d64261841a9449`.
- The installer parity repair passed **31/31** tests with the explicit local PowerShell runner and received independent review clearance.

Targeted logs: `x64/logs/hardening2-scaler-shader-red.log`, `hardening2-scaler-shader-build.log`, `hardening2-scaler-behavior-red.log`, `hardening2-scaler-green.log`, and `hardening2-installer-local-green.log`.

## Final required serial run: PASS

Command: `tools/test-rendering.sh --fg-steady-seconds 15 --report x64/logs/hardening2-qualified-rendering.json --log-dir x64/logs/hardening2-qualified-rendering`.

- Invocation: `ee577bc1e13b48a8aecc7cd0ab84f46b`.
- Started: **2026-10-27T21:05:21.952787Z**.
- Finished: **2026-10-27T21:20:11.347941Z**.
- Exit code: **0**; report status: **PASS**.
- Report: `x64/logs/hardening2-qualified-rendering.json`.
- Report SHA-256: `2f51cde7f02b94b4ec11982962ea9cf88189d37ae94450773e55b914b27dc799`.
- Log: `x64/logs/hardening2-qualified-rendering.log`.
- Retained evidence: `x64/logs/hardening2-qualified-rendering/ee577bc1e13b48a8aecc7cd0ab84f46b/`.
- All **88** retained receipt/log byte lengths and SHA-256 values were rechecked successfully after terminal PASS and again on 2026-10-28. This required-chain evidence remains intact.

| Required stage | Result | Exit code | Elapsed seconds |
| --- | --- | --- | ---: |
| Host | PASS | 0 | 158.204391489 |
| Release build | PASS | 0 | 16.471785792 |
| FG inputs | PASS | 0 | 9.173824973 |
| Moving NR | PASS | 0 | 52.416121648 |
| NR replay | PASS | 0 | 460.260418136 |
| Vulkan | PASS | 0 | 140.645814110 |
| FG provider | PASS | 0 | 51.737221051 |

The host receipt reports **94 PASS**, with zero FAIL, SKIP, TIMEOUT, ABORTED, or INCONCLUSIVE results, source head `d845b511e1b380ba2cf83d1e8635bbd62502ef16`, and `dirty=false`.

## Vulkan results and attribution

| Case | Required result | Host validation errors | Known native-model messages |
| --- | --- | ---: | ---: |
| `nr` | PASS | 0 | 2 |
| `nr-route-quiet` | PASS | 0 | 6 |
| `nr-route-quiet-wdx12` | PASS | 0 | 0 |
| `nr-route-guides` | PASS | 0 | 6 |

Validation was active. The native-model messages were attributed separately by the producer and used `VUID-vkCmdDraw-None-09600`. This run is not evidence that every component was validation-clean.

Seven native sidecars were retained automatically: four `result.json` files and three raw `validation.txt` files. The zero-message wdx12 case had no raw validation file. Their retained lengths and hashes were verified. The report's 47 matching text lines are not 47 independent validation errors.

## Sustained FG x2 qualification

The requested steady duration was 15 seconds; Ada unlock was false. Qualification returned **PASS**, with no failure reasons and **nine complete windows**, three per active phase. Every qualified window had an exact 2:1 actual/application counter ratio.

| Phase | Total application frames | Steady application | Steady actual | Steady elapsed microseconds | Complete windows |
| --- | ---: | ---: | ---: | ---: | ---: |
| Enabled | 636 | 604 | 1208 | 15064980 | 3 |
| Disabled | 48 | 16 | 16 | 318533 | 0 |
| Reenabled | 568 | 536 | 1072 | 15050219 | 3 |
| Resized | 528 | 496 | 992 | 15050476 | 3 |

Steady application counts per window were `241/185/178`, `178/178/180`, and `160/171/165`; actual counts were twice each value. Whole-phase totals include warmup and are not substituted for the steady counters.

- GPU: NVIDIA GeForce RTX 4090, vendor `0x10de`, device `0x2684`; NVIDIA driver `615.78.08`.
- CPU: Intel Core i9-13900K, 32 logical CPUs.
- Host: Linux `7.2.11-zen1-1-zen`, Python `3.14.8`.
- Provider runtime: GE-Proton11-11-x86_64; launcher SHA-256 `204d3b1dfb73f0fc50e65e226a584ddcc4ce949872723cb6358b9f04cee8fd29`.
- Output plugin SHA-256: `f4a6b2b14dcc0b1485989e430d3b4e3a44ac1800b92ba1ad74f476e64fb2b09c`.
- Provider SHA-256: `ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82`.
- `staged_binaries_unchanged=true` and `explicit_settings_unchanged=true`.
- `staged_assets_unchanged=false`: the INI was normalized with only `xefg.depthinverted=false`, `xefg.highresmv=true`, and `xefg.jitteredmv=auto` added at runtime.
- INI launch SHA-256: `ececee37dd7f053bbf223b8cb838dd4c8083730eac84cd67dcf7af0a38210183`; final SHA-256: `af54d61848b7e6dfb2cd8c8857f7b08a7b27749c6fd48342fc8541b8ac93ee23`.

This qualifies provider counter behavior on this configuration. It does not establish unique generated images, scanout, perceived smoothness, end-to-end latency, or behavior on other GPU families.

## Replay results and optional visual judgments

Required functional and seam gates passed. Of 25 recorded entries, 24 have `ok=true`; the informational `repeat-pixels-identical-frames` entry has `ok=null` and reports **256/256** frames bitwise identical across two fresh processes. The `off` control also reports 256/256 unchanged frames. Calibration used five fresh every-frame repeats.

| Optional visual judgment | Result |
| --- | --- |
| `every-b` | PASS |
| `cadence2` | FAIL |
| `cadence4` | FAIL |
| `adaptive4` | FAIL |

These failures remain visible despite the required-chain PASS. They are failures against the measured image criteria; they are not a universal human-perception verdict. The same categories failed in the initial run, and this batch does not modify the cadence algorithm. Visual equivalence of the cadence modes is not claimed.

Replay used Proton Experimental. Its retained `nr-replay/receipt.json` is 6086 bytes, SHA-256 `2931e6b566f202cee089d22aa950862fc54edb329847e87f4763bdc036b0a8dd`.

## Preserved earlier attempts

1. **Initial required chain PASS:** invocation `9e330b6c5e014c958ae83286645a8954`, source `3d125ae4`, 2026-10-27T20:18:22.848257Z through 2026-10-27T20:34:03.216430Z. Report `x64/logs/hardening2-rendering.json`, SHA-256 `56ae63b2cf4b285417ec651fe7f50fb6d2d003f3c675efc9f362fa4af3519f04`. Its old OptiScaler DLL SHA-256 is `15e1dbcbce3cc68a18b5c3446b1a08c9922ca0e1ea4a119a5841c4f50c4f9077`; the forwarder matches the final pair. All seven stages passed, but this precedes the scaler format repair. Seven omitted native sidecars were later archived with a supplemental receipt; the original report was not rewritten.
2. **Initial WorkingScale=1.25 probe FAIL:** `x64/logs/hardening2-manual/manual-scale125-2719eaf48c704ae79efa36c3ea00fe5d/report.json`, exit 1. It recorded ten host validation findings with VUID suffix `08795` and six separately attributed native-model `VUID-vkCmdDraw-None-09600` messages. The three phases built 1600x900 working targets for 1280x720 input and evaluated/presented 30/30/10 frames, with no fallback marker. This failure exposed the BCUS output component mismatch; source/SPIR-V inspection also found the NR down/FSR storage-format mismatch.
3. **First corrected required chain FAIL at host:** invocation `a22a4cb6193a438fbf47695f7fc03ae0`, clean source `7ad3f9e3`, started 2026-10-27T20:59:30.985595Z. Report `x64/logs/hardening2-final-rendering.json` records 93 PASS and one Windows-installer FAIL; build and GPU stages are `NOT_RUN`. Docker timed out and the supported local PowerShell fallback exposed an extra machine-owned `_nvngx.dll`. The extra file's SHA-256 was `dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3`. The test-only `--no-nvngx` repair isolated the common fake payload; independent fake-driver coverage remains in `tests/deploy-safety`.
4. **Interrupted/unresolved manual receipt:** `x64/logs/hardening2-manual/manual-scale125-29bf66a24e364be88c80c98936be27a4/report.json`, started 2026-10-27T21:20:55.084255Z. Fresh reads on 2026-10-28 still showed the initial `RUNNING` receipt without `finished_at` or supervision results, and no owner was observed. Preserve it as incomplete evidence; it is neither product PASS nor product FAIL.

### Preserved 125% startup attempt: TIMEOUT

The unchanged preserved helper was rerun after ownership checks. Its SHA-256 is `5aa7db09c873427f7f4ec727514407b5eac3f06e435461ad2cd5ca0b44cd006b`. The attempt exceeded its 300-second deadline and did not qualify the repaired upsampling path.

- Invocation: `7b6f97ba9b2943c48f74fcdcd0bc1ebf`.
- Report: `x64/logs/hardening2-manual/manual-scale125-7b6f97ba9b2943c48f74fcdcd0bc1ebf/report.json`.
- Started: **2026-10-28T19:30:08.731940Z**; finished: **2026-10-28T19:35:09.196701Z**.
- Report SHA-256: `752130b8376f9bde7c1a3cb63dadb1d0fd88d88ae9e8d22144d75d34f0f002bb`.
- Helper exit code: **1**. `run_suite` status: **TIMEOUT**, return code **-15**, elapsed **300.033582691 seconds**, with the deadline-exceeded note retained.
- The harness facts contain only `status=RUNNING`; there is no `OptiScaler.log` and no frame result. Successful active validation, three working-target builds, 70 evaluations/presents, and absence of scaler fallback are **not established** by this attempt.
- An absent `08795` message in an attempt without frame results is not successful regression coverage. Native-model message counts are likewise not qualified here.
- Expected, current, and staged DLL hashes match the qualified pair. Source/manual identities and the preserved helper hash remained unchanged.
- Harness SHA-256: `5eafc5339cfe4bd87a4b6769e77c34fe432968bf5204fcb32eaf69d4103612de`.
- Model SHA-256: `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`.
- GE launcher SHA-256 remained `204d3b1dfb73f0fc50e65e226a584ddcc4ce949872723cb6358b9f04cee8fd29` before and after the attempt; harness and model identities were also unchanged.
- The retained runtime log is **669 bytes**, SHA-256 `e52039189ffbb137e46de145644142f0b1f9722e06ff546764b7f01bd0a135a0`. It contains startup messages and a deprecated `VALIDATION-SETTINGS` warning, with no frame result.
- Owned harness PID `409868` and launcher PID `409759` were confirmed gone after cleanup. No global Wine termination was used.
- An attempted diagnostic backtrace was denied by the operating system with `ptrace_scope=1`. The diagnostic log is retained; no escalation was attempted.

### Preserved original-INI startup control: TIMEOUT

The 90-second control used the same fixed DLL and the original INI, without a `WorkingScale` key. Invocation `cb64309c0b2e47718f4a39ccde822a56` ran from **2026-10-28T19:37:38.741036Z** through **2026-10-28T19:39:08.827979Z** and ended in **TIMEOUT**, with supervision return code **-15** after **90.025890322 seconds**. Its facts remained `RUNNING`, no `OptiScaler.log` was produced, and artifact identities were unchanged. Report SHA-256: `bb97c3b52109bb33f65bac0833aa0ecb12360ce9510776cd1151c10aaa7e41be`.

The restarted Winx shell had neither `DISPLAY` nor `WAYLAND_DISPLAY`, and reported `XDG_SESSION_TYPE=tty`. The user's running Hyprland, Waybar, and Xwayland processes exposed the existing graphical session; UID 1000 sockets `/tmp/.X11-unix/X0` and `/run/user/1000/wayland-1` were live. These are observed environment differences. The two timeout receipts alone do not prove a scaler defect or establish the cause of startup failure.

## Final WorkingScale=1.25 regression: PASS

The exact preserved helper was launched again with only process-local variables for the existing session: `DISPLAY=:0`, `WAYLAND_DISPLAY=wayland-1`, `XDG_SESSION_TYPE=wayland`, `XDG_RUNTIME_DIR=/run/user/1000`, and `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`. No code or display configuration was changed.

- Invocation: `6825c786d5e1430895c04c7fa2c8254a`.
- Started: **2026-10-28T19:39:43.171372Z**; finished: **2026-10-28T19:40:25.839354Z**.
- Report: `x64/logs/hardening2-manual/manual-scale125-6825c786d5e1430895c04c7fa2c8254a/report.json`.
- Report SHA-256: `9243fbfb0901f7abd21cbddf4b12168c8e44864fe497ebb44b5db81c6c72efd9`.
- Helper exit code: **0**; findings: **[]**. Supervision: **PASS**, exit code **0**, run ID `c19ca9676632493ca6079125d3e9a05c`, elapsed **42.267317879 seconds**.
- Validation was active with **zero host errors**. The six native-model messages were exclusively `VUID-vkCmdDraw-None-09600`; no `08795` finding appeared in the retained raw validation evidence.
- The helper confirmed three **1600x900** working-target builds for **1280x720** input, with no forbidden fallback marker and `up_leg_execution_source_backed=true`.
- DLL, forwarder, harness, model, and Proton identities remained unchanged across all expected/current/staged checks. The exact helper was snapshotted and verified for every manual attempt.

| Active phase | Evaluations | Presents | Readbacks reported identical |
| --- | ---: | ---: | ---: |
| `on` | 30 | 30 | 28 |
| `on-again` | 30 | 30 | 28 |
| `on-at-exit` | 10 | 10 | 8 |

These phases account for 70 active evaluations and presents. The overall 399 presents also include cold and disabled phases; that total is not the active-evaluation count. The probe does not confer image-quality approval.

| Retained log | Bytes | Verified SHA-256 |
| --- | ---: | --- |
| Runner | 198 | `16f86227e9667d610b205a6c3a13cebbc48820b95e21299d41234969c8258bc4` |
| OptiScaler | 96265 | `08be0f033f05a74301a11d5cf09190daadca47d38bf15a450da45280ee76d3f3` |
| Validation | 5147 | `3a8b4041d5b0707260afc26eb50bf4027af97b2c51486b1c194a683f5222db0d` |

`environment-recovery.json` beside the final report records the environment observation and process-local recovery. The same preserved helper completed after receiving the existing graphical session's variables. No original report was rewritten, and no code or display configuration was changed for this recovery.

## Scope limits

The successful manual probe covers its source-backed upsampling path only. Independent GPU coverage of the downsampling leg, repeated command-buffer submission, exposure-meter behavior, every filter's image quality, native Windows, and FG x3/x4 remains outside this qualification. Host behavior tests and SPIR-V checks do not substitute for those runtime measurements.

The three optional cadence visual failures and the known native-model validation messages remain open evidence. No general FPS, latency, image-quality, or cross-hardware improvement is claimed from this batch.

## Integration and retained delivery evidence

The qualified production source is `d845b511e1b380ba2cf83d1e8635bbd62502ef16`. The subsequent verification and plan commit must be documentation-only. The delivery target is `dlss-neural-rendering`, integrated by fast-forward after a fresh branch/cleanliness check. This record establishes qualification; it does not itself assert that integration has executed.

Delivery must preserve a backup of the previous primary artifacts and promote the exact qualified DLL pair listed above, with hash verification. The required-chain evidence, original failed and incomplete attempts, final manual PASS, helper snapshots, and environment-recovery record must remain inspectable without rewriting their original bytes or provenance.

After execution, the delivery receipt is recorded in the primary repository at `x64/logs/hardening2-integration-*/manifest.json`. It captures the final source head, backup location and identities, promoted binary identities, copied evidence, and final Git/integration status. This manifest is the authority for the completed delivery state.

No push, game deployment, model download, global Wine termination, or display modification is part of this finalization.
