# Vulkan core shutdown reliability plan

> **For agentic workers:** Use superpowers:executing-plans and test-driven-development. One implementer; the coordinator owns GPU/compiler admission and independent review.

**Goal:** Centralize the Vulkan driver-core Shutdown1 ABI adapter so the public input and NR present host both call the correct internal signature.
**Architecture:** Preserve the SDK's public one-device callable; represent the private driver-core slot/load as device plus initialized writable output count. Follow the integrated D3D11/D3D12 adapters, and update the existing NR host caller rather than leaving its reinterpret_cast around a public wrapper.
**Tech Stack:** C++20/MSVC, Python host fixtures, ASan/UBSan, Wine/Proton for scoped runtime evidence.
**Spec:** The contract below implements the user's approved first reliability item and the source audit at base 5faa8dce75091538408b205c182b47e244f3bf4e.

## Global constraints and contract

Work only on fix/vk-core-shutdown-20261030. Main and other worktrees are read-only. No commit, merge, push, deployment, game launch, remote-PC access or nested agents. No shader/config/default/per-frame changes. Preserve existing lifetime policy and public SDK/custom interfaces. The measured core SHA-256 dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3 uses a two-argument Vulkan export (RVA 0xd640), forwarding the second argument to shared shutdown RVA 0x3b820. This is evidence for that core, not every driver. The current public input calls the raw one-argument proxy; NR host compensates using a local two-argument cast. Both consumers must remain correct after centralization.

## Review focus

Preserve public one-argument signatures, private slot/load casts, return propagation, null-device fallback, unavailable or uninitialized entries, and the NR host's existing orderly device teardown. Do not retain a two-argument cast of the new one-argument wrapper. A host fake must write through the output pointer; source text assertions alone do not prove the runtime ABI. No successful shutdown by skipping the call. Do not label a direct core call as public DLL export coverage.

## Task 1: adapter and host regressions

Files: OptiScaler/proxies/NVNGX_Proxy.h; retoque/dlssnr/DlssNrFeature_Vk.cpp shutdown helper only; tests/nr-shutdown/{run.py,cases.h,README.md}; affected checks in tests/nr-vk-present-host/run.py; dated addition to docs/dlssnr/design/ngx-shutdown-order.md.

- [x] Coordinator created isolated worktree and ran baseline nr-shutdown and nr-vk-present-host: two PASS.
- [x] First extend existing host tests to compile the actual Vulkan public/private types, real raw slot/load/getter and host call path. Observe meaningful RED before changing production.
- [x] Add minimal private core typedef/slot/loader adapter; keep the public getter type. Update NR host to use the adapter with its public signature, without a replacement private cast.
- [x] Verify initialized writable count, forwarded device, success/error results, null and unavailable-entry behavior; retain D3D11/D3D12/custom cases.
- [x] Run focused suites and format touched C++ regions with clang-format 22. Preserve RED/GREEN logs; never add sanitizer suppressions to acceptance code.

## Task 2: runtime preparation and review

- [x] Identify or add a bounded DLL-backed Vulkan shutdown probe using public exports, not a new production test export. Reuse the existing Vulkan fixture/setup where suitable. Require proven initialization, correct loaded DLL, actual Shutdown1 return and process exit. Include the NR host teardown as a separate case; do not substitute one for the other.
- [x] Run only native host tests as implementer. Submit exact safe build/runtime commands and expected proof to the coordinator; no Wine/GPU until explicitly admitted.
- [x] Independent reviewer checks full diff and tests. Coordinator runs host layer, Release build and feasible before/after runtime cases serially with retained hashes/logs.
- [x] Add dated evidence summary with exact limits. Leave changes uncommitted and stop; no unattended continuing work.

## Outcome (2026-10-30)

Host: `tests/nr-shutdown` and `tests/nr-vk-present-host` PASS (RED: compile rejection of the old host cast and an
ASan fault in the fake core with the one-argument slot; GREEN after the adapter). The coordinator's combined host run
passed 100/100 on stable sources. Runtime, serially under the lease: baseline before-2 `nr-route-quiet` exit 5;
after-2 runner PASS but unjudged (executor lost its receipts); after-3 judged PASS for the host (`nr-control`) and the
public input (`nr-route-quiet` with the SR linked). Codex's final judge review: no blockers. Details, hashes and limits:
[Vulkan evidence record](../reviews/2026-10-30-vk-core-shutdown.md). Uncommitted; native Windows and games remain open.
