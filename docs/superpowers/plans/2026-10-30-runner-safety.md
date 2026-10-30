# Legacy sweep and runner safety implementation plan

> **For agentic workers:** Use superpowers:executing-plans and test-driven-development. One implementer; the coordinator owns compiler/GPU admission and review.

**Goal:** Prevent false successful sweep results, unsafe GPU resource reuse, and recovery that terminates another agent's work.
**Architecture:** Reuse the existing owned-process supervisor and common prefix lock from tools/build_local.py rather than introducing another supervisor. Make the legacy sweep track actual scenario success and demand confirmed GPU completion before reuse/release. Keep changes in tests/tooling, not the renderer.
**Tech Stack:** Python 3, C++20 test harness, existing native subprocess fixtures, MSVC/Wine validation admitted separately.
**Spec:** Contract below implements the user's approved reliability items 2 and 3 at base 5faa8dce75091538408b205c182b47e244f3bf4e.

## Global constraints

Work only on fix/runner-safety-20261030; main and other worktrees are read-only. No commit, merge, push, deployment, game launch, remote-PC access or nested agents. No production renderer, shaders or shipped INI changes. Do not weaken success requirements or disable sanitizers. Keep suite IDs stable, add regressions to existing registered suites. Never run the old dangerous recovery for a real reproduction; simulate owned/unrelated processes in host tests. No Wine/GPU until coordinator admission.

## Review focus

Every evaluation refused after an earlier success must still fail the sweep. Successful scenario coverage is distinct from attempt count. Signal/SetEventOnCompletion/wait/fence completion failures must not reach allocator Reset, resource Release, or RAII unwinding of GPU-owned references. Compiler lock admission must be race-free, held exactly once (including replay/moving modes), and released on error/interruption; unavailable ownership must fail closed. Never kill by executable name or reset a shared wineserver. Retain diagnostic output rather than silently retrying. Fake-compiler tests must verify an unrelated same-name process remains alive.

## Task 1: strict legacy sweep

Files: tests/dlssnr-loopback/harness.cpp and a small test-local helper if needed; regression cases under tests/dlssnr-loopback; tests/nr-moving-evidence/run.py and README for wiring; tests/dlssnr-loopback/README.md.

- [x] Coordinator created isolated worktree and ran nr-moving-evidence, build-lifecycle and runner-results baseline: three PASS.
- [x] Write failing regressions for initial and late evaluation failure, missing scenario success, failed queue signal/event setup, wait timeout/failure, device loss/completion below target. Compile real helper/control logic with host fakes, rather than only asserting source strings.
- [x] Count attempts versus success per step; require every planned step, fail on any refused evaluation and check feature/cleanup errors. A completed earlier composition cannot rescue a later failed step.
- [x] Establish a bounded checked completion operation before reuse or release. On unknown completion, flush failure evidence and terminate without unsafe automatic cleanup; do not replace GPU proof with sleep.
- [x] Keep other harness modes unchanged by default. Run focused native tests; retain RED/GREEN evidence.

## Task 2: shared process supervision

Files: tests/dlssnr-loopback/run.py; tools/build_local.py only minimal reusable lock interface if justified; tools/lsfg-study/harness/run.py; tools/lsfg-study/ctxbench/run.py; associated registered build-lifecycle/nr-moving-evidence tests and docs.

- [x] Add failing fake-process tests for lock contention, same-name unrelated process survival, owned descendants on timeout/interruption, release of lock, no retries and preserved failure logs. Test entrypoints, not just the already-tested supervisor in isolation.
- [x] Remove legacy name-based process termination, wineserver -k and prefix resets from these entrypoints. Reuse run_step/owned-process supervision; preserve bounded timeouts and propagation of nonzero results.
- [x] Coordinate exactly one prefix lease for build/run modes; account for existing run_moving.lease and skip-build. Avoid non-reentrant nested flock and check-then-spawn races.
- [x] Focused suites, syntax and diff checks passed; code frozen for review. The real harness commands and results are recorded in the [runtime evidence](../reviews/2026-10-30-runner-safety-runtime.md).

## Task 3: independent verification

- [x] Independent cross-review by Claude: no blockers; minor observations remain outside this scope. The review is retained in the reliability evidence archive.
- [x] Combined native host: 100 PASS with intact sanitizers. Serial private-prefix compiler, seven-step sweep and six-variant replay passed; see the [runtime evidence](../reviews/2026-10-30-runner-safety-runtime.md). Host and Proton evidence do not prove native Windows or game behavior.
- [x] Document coverage and limits with relative evidence references and hashes; work remains uncommitted.
