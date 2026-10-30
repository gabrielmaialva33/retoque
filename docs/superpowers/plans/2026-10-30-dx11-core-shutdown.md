# D3D11 core shutdown adapter implementation plan

> **For agentic workers:** Use superpowers:executing-plans for the bounded implementation; the coordinator owns the runtime gate and review. Track completion below.

**Goal:** Fix the reproduced D3D11 Shutdown1 core-ABI mismatch without changing the public SDK interface or mixing it with renderer extraction.
**Architecture:** Follow the existing D3D12 adapter: a private two-argument core function pointer, local writable initialized output, and a public one-argument adapter/getter. Keep normal feature cleanup and per-frame behavior unchanged.
**Tech Stack:** C++20/MSVC, Python host fixtures, ASan/UBSan, Proton.
**Spec:** This plan's Contract below, informed by [the existing D3D12 ABI design](../../dlssnr/design/ngx-shutdown-order.md) and the retained 2026-10-29 before/after D3D11 reproduction.

## Contract and global constraints

Base dd2a589108652870a702313678cf67cf8a98ced7. Work only on fix/dx11-core-shutdown-20261030. The present-guide extraction worktree and primary checkout are read-only. No commit, merge, push, deploy, game launch or remote-PC operation. No shaders, settings, frame-generation, default-path or lifetime-policy changes. Do not bypass Shutdown1 to obtain PASS. The user authorized the separate fix and verification.

The observed core SHA-256 is dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3 (32.0.16.1752). Its raw D3D11 Shutdown1 saves argument two and writes through it at RVA 0x3b8b4. Before and extracted DLLs both reproduce the same crash after 200 evaluations/presents. This establishes this binary's ABI, not every NVIDIA version. Preserve public PFN_D3D11_Shutdown1 and SDK exports; use the private type only for the raw driver-core slot/load. Null-device behavior follows the existing adapter pattern: use all-devices shutdown or NotImplemented; absent/uninitialized entries stay unavailable. Preserve return values.

## Review focus

- Public one-argument SDK/custom interfaces must not inherit the private ABI.
- Fake core must actually write through the non-null, initialized second argument.
- Uninitialized runtime, missing device export, null device and missing all-devices export remain safe.
- Success/error results propagate unchanged; existing D3D12/custom-runtime cases remain green.
- Runtime acceptance requires the actual fixed DLL, 200 evaluations/presents and complete cleanup with zero process exit, not just positive guide counters.

## Task 1: Fix and regression tests (one implementation writer)

Files: OptiScaler/proxies/NVNGX_Proxy.h; tests/nr-shutdown/{run.py,cases.h,README.md}; dated addition to docs/dlssnr/design/ngx-shutdown-order.md. No unrelated production files.

- [x] Coordinator checked clean primary base and created isolated worktree; existing nr-shutdown baseline passed.
- [x] Extend the existing suite first, compile actual D3D11 production types/getter/adapter with fakes; observe a meaningful RED on the original code. Guard the raw slot and loader cast too, not a handwritten fake-only replacement.
- [x] Add the smallest private core adapter preserving the public signature, with null and unavailable-entry handling matching D3D12.
- [x] Observe GREEN with initialized writable output, device identity, error propagation and unavailable/null paths; preserve prior D3D12/custom tests. Retain RED/GREEN logs without disabling production sanitizers.
- [x] Document exact scope and evidence limits, format only affected C++ regions and stop editing. No build/GPU by the implementer.

## Task 2: Independent validation (coordinator and Claude)

- [x] Review full diff against the contract, including type-loading boundary and regression strength.
- [x] Full host layer: 100 PASS; evidence: `artifacts/dx11-shutdown-final/host/summary.json`.
- [x] Independent review completed without blockers; tested source snapshots are identified in the validation review.
- [x] Final metadata checks: docs, privacy and nr-shutdown 3/3 PASS; `artifacts/dx11-shutdown-metadata/summary.json`, run ID `92294dd89f8b4ad48bacc77436a78035`.
- [x] Release build PASS; evidence: `artifacts/dx11-shutdown-final/stages.json`. Preserve hashes and logs.
- [x] Fix-only runtime PASS: 200 evaluations and 200 presents, Shutdown1 and all releases returned, process exit 0; no `c0000005` or unhandled exception in trace. Baseline reproduced FAIL with the same inputs. Evidence: `fix-only-runtime/result.json` in the retained D3D11 shutdown evidence directory.
- [x] Combination validated in an independent worktree: host, Release build, two D3D11 runs, D3D12 present and replay. Results and limits are recorded separately in the validation review.
- [x] Record observed gates and missing native-Windows/game evidence in the [validation review](../reviews/2026-10-30-dx11-core-shutdown.md). Changes remain uncommitted; main is untouched.

## Coordination ledger

Pre-flight: Task 2 consumes Task 1's frozen diff. One code writer; coordinator alone owns compiler/GPU admission. Existing refactor is never a mutable baseline. Dependencies may be copied locally but not changed or staged. No live work continues after the response.
