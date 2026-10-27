# Build lifecycle regressions

Run from any directory on native Linux with Python 3.11 or newer:

```sh
python3 tests/build-lifecycle/run.py
```

The suite uses temporary checkouts and real fake-compiler subprocesses. It never
starts Wine, accesses the real compiler prefix, or modifies a game installation.
The only fault injection replaces the second `os.replace` operation while the
publication test performs the first promotion and rollback against real files.

Coverage includes refusal of an already-held prefix lock before stamp/log/compiler
work; CLI configuration, extra argument and environment propagation; both projects
and output files; first/second compiler failure; missing/stale output detection;
valid incremental forwarder output; invalid destination and second-promotion
rollback, including SIGTERM delivered immediately after the first real replacement; inherited-lock prevention; SIGINT/SIGTERM cleanup and lock release;
CPU and log activity, including short workers reaped between polls; owned wineserver CPU excluded from build progress; hard and idle timeouts; owned child and observed detached
child cleanup; an unrelated process with the same executable; background children
left behind after successful commands; and failed executable launch evidence.

The normal entry point remains:

```sh
tools/build-local.sh [Configuration] [MSBuild arguments...]
```

`MSVC_BIN`, `WINEPREFIX` and `WINEDEBUG` retain their previous defaults. The
nonblocking prefix lock is `$XDG_RUNTIME_DIR/msvc-prefix.lock`, or
`/run/user/<uid>/msvc-prefix.lock` if `XDG_RUNTIME_DIR` is unset/empty. A held lock
returns 75 before build stamps or logs change. The lock file descriptor is not
inherited by compiler children. The runtime directory must already exist.

Each invocation retains its own `x64/logs/<timestamp>-<pid>-<id>/` directory with
one file per project that was started, including failures. Compiler stdout and
stderr go directly to a regular file; Wine never receives a stdout pipe from
this entry point. The files are replayed after each step. Both projects must
succeed before the pair is prepared and published to `x64/out/`. Each file
replacement is atomic and ordinary promotion errors roll back earlier
replacements. SIGINT/SIGTERM are blocked around each replacement plus its rollback record and while restoring prior files. The pair is **not** atomic for concurrent readers, and abrupt
process/system failure between replacements can leave a mixed pair.

Changing OptiScaler resource stamp contents requires the produced OptiScaler DLL
to have changed filesystem metadata during this invocation. Successful no-op
compilation cannot silently republish the prior DLL after stamp changes. A valid
incremental forwarder DLL may remain unchanged because the forwarder does not
consume these stamps. Missing or ambiguous output paths are errors. Compiler
success remains the evidence that a legitimately unchanged incremental output
matches the requested build; the wrapper does not independently implement
MSBuild's dependency graph.

The Python `run_step` helper accepts explicit inactivity/hard timeouts, polling
interval, termination grace and optional own/waited-child CPU-contributor predicates, allowing the suite to test short deadlines. The generic helper counts all owned CPU by default; the build selects owned MSBuild/CL/link workload processes and ignores Wine service CPU. Waited-child CPU detects short worker churn between polls; in build mode only actual MSBuild/CL/link executables can contribute it, so the shell wrapper cannot credit reaped Wine infrastructure. A spinning wineserver with idle compilation and no log output therefore remains a hang. The
normal defaults are 90 seconds without owned CPU/log progress, a two-hour hard
limit per project, 0.2-second polling and a two-second TERM grace. Timeout returns
124; interruption returns 128 plus the signal number. Remaining owned processes
receive TERM and then KILL with bounded waits. Normal child exit also cleans up
owned background work before releasing the prefix lock.

Linux pidfd support is required and checked before spawning; the supervisor refuses unsupported kernels instead of falling back to PID-only signals. Cleanup opens a stable pidfd, then rechecks the recorded start time before signalling through that fd, closing the PID-reuse race.

Ownership uses `/proc` PID/start-time identities, the newly created process
session/group, and descendants whose ancestry was observed. Adopting a new process
from group membership requires a still-live previously known identity in that
original group/session. A vanished group is never resurrected by a reused PGID. An observed child
remains owned after `setsid` or reparenting. Processes that escape their group
and ancestry **before any observation** cannot be proven owned; existing shared
Wine services are never adopted by executable name. A process blocked in an
uninterruptible kernel wait may survive SIGKILL; this is reported as an error.
No automatic retries, `wineserver -k`, shared-prefix reset, or global executable
name kills are performed. Diagnose the retained logs and coordinate any manual
prefix recovery through the same lock before trying another build.

Some command sandboxes virtualize `os.getpid()` independently of `/proc`. The
supervisor refuses such an environment before spawning or signalling work. The
suite explicitly skips process-dependent cases there; skips do not establish
ownership coverage. Run the full suite on native Linux before trusting changes
to process supervision.
