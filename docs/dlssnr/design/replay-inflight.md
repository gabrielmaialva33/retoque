# In-flight replay stress (harness only)

The synchronous replay certifies each output immediately and Resets its list at the
next evaluation. Add opt-in LOOPBACK_REPLAY_INFLIGHT=2..4 (0/default stays synchronous).
Each slot owns its allocator/list, all upload/texture/readback/query resources and
its pending outcome. Reuse waits for that slot's output fence before mapping,
writing files, resetting its allocator or overwriting uploads. Lists Reset only
on slot reuse, N evaluations after capture. Drain every slot before teardown.

Completion timing must be repeatable: before each ExecuteCommandLists the queue
waits on a harness CPU-signalled gate. On slot reuse release that oldest serial
and wait for its output fence. Newer submissions stay blocked until their turn;
no submit performs a CPU wait. This makes N-1 outstanding submissions observable
at steady-state evaluation rather than depending on disk/CPU/GPU timing. It is
one queue, not multi-queue or arbitrary engine scheduling evidence.

Optional LOOPBACK_REPLAY_CANCEL_EVERY (0/off or >=16) leaves each selected NR list
closed and unsubmitted until its slot returns; Reset then discards it. A separate
copy-only list uploads the original planes/output and records its fenced output
readback, so a cancelled frame still has actual GPU-certified input pixels. Its
NR outcome is retained separately from executed=false. No NR commands from that
list execute. Cancellation is a diagnostic discard, not a successful NR frame.
LOOPBACK_REPLAY_RECREATE_EVERY (0/off or >=16) releases/replaces a selected slot's
previous list after its fence completes, without Reset, exercising orphan refresh.
Cancelled lists always Reset rather than release. Allocate the replacement before
releasing the old pointer to avoid accidental pointer-identity reuse.

Manifests include mode, recorded/executed/cancelled and scheduling evidence. Reject
mode/settings/capture/frame identity mismatches at baseline comparison. Stress
admits expected policy skips but must execute NR on scored frames; missing outputs,
fence failures, invalid schedules and unexpected failure outcomes fail. Same-mode
baselines use the original frozen DLL and are repeated in fresh processes.
Require bitwise synchronous output for the prefix with identical admission/submission
history (and the whole run when histories match). Once a decision differs, temporal
model history differs even at later locally matching decisions. Report these differences, never silently treat them as pixel
coverage. Same-mode comparison still requires all decisions and pixels identical.

Preserve synchronous behavior and prove it against the existing frozen variants.
Stress sets are stored separately under baseline/inflight/<mode>/runs; do not modify
existing corpus, DLL or synchronous manifests. Judge/calibration are not meaningful
for deliberately discarded/scored skipped frames and are disabled in stress mode.
