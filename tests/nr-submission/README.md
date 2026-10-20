# NR submission model tests

Run `python3 tests/nr-submission/run.py` from the repository root. Requires g++ with
C++20, pthreads, ASan and UBSan. A pass requires exit zero and both PASS lines;
assertion, sanitizer or boundary-guard failure is a failed test.

Tests compile the production portable lifetime model. They cover missing/delayed
submission, duplicate execution/replay, multiple queues, Reset before fence
completion, failed Reset, failed Signal, device-loss UINT64_MAX, exact-epoch
notification across Reset, bounded admission, 3000 retirements and 1000 concurrent
Reset/notification pairs under the caller's mutex contract. The in-flight rules of
`docs/dlssnr/design/chain-in-flight.md` are covered too: completed-but-unsealed epochs
are done for admission at once and finished for freeing only after the replay window (never
in strict mode); a valid fence still outstanding is never finished however old; a recording
waiting `RingTicks` for its first submission refuses new work until it runs, and a late
submission makes it count again; released (orphaned) lists are safe when completed or never
executed, not while running; a failed Signal stops holding admission after `AgeOutTicks`
but never settles retirement; cancelled recordings; the in-flight limit and a second
recording into an unsubmitted list; mixed queues, the queue-switch latch and an unknown
queue after a known one; replay inside the window keeping parked objects unsettled until
Ready, and pruning outside it; capacity-pressure pruning on one persistent Usage; a
duplicate inside one batch latching strict; and reclaiming, never in strict mode and never
of a referenced epoch. A source guard checks
both actual queue-hook paths notify after their original ExecuteCommandLists.

These tests do NOT execute COM, Detours, D3D12, NGX or GPU work. Hook coverage,
unknown wrapper implementations and real queue/device-loss behavior require
separate integration validation. No game installations or markers are accessed.
This directory is outside the release package's explicit artifact allow-list.
