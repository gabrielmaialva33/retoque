# Suite runner result regressions

Run on native Linux with Python 3.11 or newer:

```sh
python3 tests/runner-results/run.py
```

The tests use temporary checkouts and controlled Python subprocesses. They do
not start Wine, compile rendering code, or access a game installation. The
process-cleanup cases must run on a native process namespace. Do not use a
PID-virtualizing command sandbox to validate process signalling.

## Result contract

An opted-in suite declares `result = "artifacts/result.json"` in the manifest.
Its terminal result must use schema `retoque-suite-result`, integer version 1,
the current `RETOQUE_TEST_RUN_ID`, and a recognized terminal status. A missing,
stale, malformed, nonfinite, or RUNNING result fails that suite. Optional fields
consumed by the runner have explicit types: string `reason`, `updated_at`, and
`terminated_by`; a string list `missing`; and boolean `runtime_coverage`.

A nonzero child exit always fails the suite. Valid current structured evidence
is retained in the aggregate even then. Prelaunch skips/aborts and decisions
written by the runner identify their evidence source as `runner`; ordinary
legacy suite success remains `process-exit` evidence. An empty selection emits
a fresh INCONCLUSIVE aggregate instead of leaving an older report intact.

Names used for log files are safe identifiers. Result paths are validated both
before launch and immediately before reading or writing after execution, so a
suite-created symlink cannot redirect those operations outside its directory.

## Interruption boundary

POSIX supervision requires default SIGCHLD and the runner as the exclusive
reaper of its own subprocesses. A nondefault SIGCHLD is rejected before launch;
the runner does not change this process-wide handler in worker threads.

On interruption or timeout, the runner sends TERM to its subprocess group and
allows four seconds for fixture cleanup. It keeps the leader unreaped until
after the final group signal, reserving the leader PID against reuse. A leader
whose return code is already known is never used for group signalling. Linux
procfs may prove the group empty and shorten the grace period; POSIX systems
without usable procfs use the entire grace period before KILL and wait.

Fixtures still own cleanup of children that leave their process group. A
regression fixture waits three seconds for a detached child before killing it;
the runner must allow that cleanup to finish. The test cleans its detached
child even if an assertion fails. This does not claim ownership of arbitrary
detached processes or shared Wine services.

## Review regressions

The added tests cover optional-field errors and nonfinite values; log-name
containment; empty selections; post-execution result symlinks on normal and
timeout paths; prelaunch evidence sources; retained nonzero-exit evidence;
report-write failures stopping active workers before executor shutdown;
three-second detached-child cleanup; and refusing unsafe early/reaped-leader
group signalling. The injected report-write failure leaves actual suite
subprocesses and their signal handling intact.

## Terminal report completion boundary

The runner observes SIGINT/SIGTERM through the first terminal report write.
After that write returns it seals signal observation and snapshots the signal
already received. If interruption arrived during that write, it atomically
writes ABORTED before returning 128 plus the captured signal number. Later
signals handled during the remaining finalization cannot change the selected
exit code or report status retroactively. This applies to both normal results
and an empty selection; it uses no process-wide signal mask in worker threads.

Four deterministic regressions invoke the installed handler directly during the
terminal writer or during the final print after the completion boundary. They
cover PASS and empty-selection INCONCLUSIVE reports, use real report files, and
do not send process-directed signals.
