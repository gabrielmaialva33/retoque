# Rendering hardening: first implementation batch

Status: approved in the conversation on 2026-10-27; implementation on an isolated branch.

## Intent

Make builds and test results dependable, then close the known Vulkan recording-resource hazards and prepare evidence-based x2/x3/x4 qualification. The user approved the proposed first batch; no further planning approval is needed.

## Constraints

- Preserve the primary checkout; no merge, push, game deployment or workstation display changes.
- Do not download or redistribute model/provider assets.
- Keep upstream-owned paths stable and prefer fork-owned helpers.
- Use the existing shared compiler-prefix lock. Supported build entry points must acquire it themselves.
- Stop only processes owned by the operation. A process name is not proof of ownership.
- Unknown GPU completion or recording lifetime never becomes safe merely because time passed.
- Preserve explicit configuration and default-off behavior.
- Test results identify executed coverage, not merely successful process exit.
- Native Windows and RTX 3060 claims require execution on that actual environment.

## Build and test delivery

The build entry acquires the compiler-prefix lock before stamps, logs or compilation change. Busy means an explicit nonzero result; nested external locking is documented as unsupported rather than silently bypassed. A bounded supervisor owns each subprocess group, retains attempt logs and reports hang/failure without broad process-name termination. Prefix-wide reset is permitted only with provable ownership; otherwise stop safely and preserve diagnostics.

The test runner retains standalone suites and the current registry. Suites with a declared structured result must emit the current run ID. Missing, malformed, stale or nonterminal results cannot become PASS. Exit failure overrides a claimed PASS. The initial structured adopters are the D3D12 FG input and provider fixtures. Legacy suites remain labeled as process-exit evidence. Persistent aggregate reports distinguish PASS, FAIL, SKIP, TIMEOUT, ABORTED and INCONCLUSIVE. Interruptions terminate owned subprocesses and produce a terminal aggregate result.

## GPU delivery

First reproduce V-06 (descriptor/constant reuse before submission) and V-02 (a generation referenced by recorded or resubmittable work). Record a focused GPU design before changing behavior. A fix must preserve recording references and pending execution references separately, and must bound resource growth with admission refusal before partial command recording. Test cancellation/reset, delayed submission, legal resubmission, resize and device loss. A fence for one submission alone is not a complete lifetime proof.

D3D12 caches must be device-associated; depth-copy format/plane/MSAA compatibility needs explicit validation, with native-Windows limits reported.

## Qualification delivery

Extend the existing production-provider evidence toward requested/effective multipliers and observable presentation counters, with exact runtime identities and explicit coverage limits. Keep RTX 3060 native-Windows rows pending until that hardware is available. Results from the local RTX 4090 are not Ampere qualification.

## Decisions

Implement the portable build/test fixes first, independently testable and reviewable. GPU fixes receive a separate focused plan after source-level investigation. Keep a progress ledger so later turns continue the same batch rather than repeat finished work.
