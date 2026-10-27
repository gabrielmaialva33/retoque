#!/usr/bin/env python3
"""Linux-only local build lifecycle: prefix lock, owned work, retained evidence."""
from contextlib import contextmanager
from datetime import datetime
import fcntl
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid


class Interrupted(BaseException):
    def __init__(self, signum):
        self.signum = signum


def _interrupt(signum, _frame):
    raise Interrupted(signum)


@contextmanager
def _signals(handler):
    previous = {}
    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.signal(signum, handler)
        yield
    finally:
        for signum, old in previous.items():
            signal.signal(signum, old)



@contextmanager
def _blocked_interrupts():
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def _require_pidfds():
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('Linux pidfd signalling is required for safe process cleanup')
    try:
        descriptor = os.pidfd_open(os.getpid())
        try:
            signal.pidfd_send_signal(descriptor, 0)
        finally:
            os.close(descriptor)
    except OSError as exc:
        raise RuntimeError(f'cannot obtain pidfd for safe process cleanup: {exc}') from exc


def _process(pid):
    """Return (start, parent, group, session, own CPU, state, waited-child CPU)."""
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()
        return (int(fields[19]), int(fields[1]), int(fields[2]), int(fields[3]),
                int(fields[11])+int(fields[12]), fields[0], int(fields[13])+int(fields[14]))
    except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError, ValueError):
        return None


def _snapshot():
    result = {}
    for item in Path('/proc').iterdir():
        if item.name.isdecimal():
            value = _process(int(item.name))
            if value is not None:
                result[int(item.name)] = value
    return result


class OwnedProcesses:
    """Identity records survive reparenting; executable names never establish ownership."""
    def __init__(self, child):
        self.child = child
        self.pgid = child.pid
        self.identities = {}
        self.root_start = None
        root = _process(child.pid)
        if root:
            self.root_start = root[0]
            self.identities[child.pid] = root[0]

    def observe(self):
        snapshot = _snapshot()
        # Only a current, matching identity may establish a child's ancestry.
        proven = {pid for pid, start in self.identities.items()
                  if pid in snapshot and snapshot[pid][0] == start}
        # Group membership can extend a current observation, but cannot
        # resurrect ownership once every known live group anchor disappeared.
        anchored = any(snapshot[pid][2] == self.pgid and snapshot[pid][3] == self.pgid
                       and snapshot[pid][5] != 'Z' for pid in proven)
        if anchored:
            for pid, record in snapshot.items():
                if record[2] == self.pgid and record[3] == self.pgid:
                    proven.add(pid)
        changed = True
        while changed:
            changed = False
            for pid, record in snapshot.items():
                if pid not in proven and record[1] in proven:
                    proven.add(pid)
                    changed = True
        for pid in proven:
            self.identities[pid] = snapshot[pid][0]
        return {pid: snapshot[pid] for pid in proven if snapshot[pid][5] != 'Z'}

    def send(self, signum):
        for pid, start in tuple(self.identities.items()):
            # Open the stable kernel handle first, then check start time. A PID
            # recycled before open fails the check; recycling after it cannot
            # retarget the pidfd to a different process.
            try:
                descriptor = os.pidfd_open(pid)
            except ProcessLookupError:
                continue
            try:
                current = _process(pid)
                if current is not None and current[0] == start and current[5] != 'Z':
                    try:
                        signal.pidfd_send_signal(descriptor, signum)
                    except ProcessLookupError:
                        pass
            finally:
                os.close(descriptor)

    def cleanup(self, grace, poll):
        # Ignore repeated terminal signals while the bounded cleanup is in progress.
        with _signals(signal.SIG_IGN):
            for signum, duration in ((signal.SIGTERM, grace), (signal.SIGKILL, max(grace, .2))):
                end = time.monotonic()+duration
                while True:
                    live = self.observe()
                    if not live:
                        break
                    self.send(signum)
                    if time.monotonic() >= end:
                        break
                    time.sleep(min(poll, max(0, end-time.monotonic())))
            try:
                self.child.wait(timeout=max(grace, .2))
            except subprocess.TimeoutExpired:
                # Uninterruptible kernel waits cannot be bounded by userspace signals.
                pass
            return self.observe()


def run_step(command, *, cwd, log_path, env=None, inactivity_timeout=90.0,
             hard_timeout=7200.0, poll_interval=.2, term_grace=2.0, cpu_progress_filter=None,
             waited_cpu_progress_filter=None):
    """Run one command with file output, returning normal status, 124, or 128+signal.

    CPU deltas of owned identities or a changed log size reset the idle deadline.
    cpu_progress_filter(pid), when set, selects which owned CPU deltas count;
    it never limits ownership, ancestry discovery, or cleanup.
    Waited-child CPU accounts for short workers between polls. By default it
    follows the CPU filter; a separate predicate can exclude wrapper reaping.
    All descendants observed during polling remain owned after setsid/reparenting.
    The hard deadline bounds continuously active work as well. On every terminal
    path remaining owned processes get TERM then KILL; logs are never discarded.
    Call from the main thread (signal handlers are process-wide).
    """
    if min(inactivity_timeout, hard_timeout, poll_interval, term_grace) <= 0:
        raise ValueError('timeouts and poll interval must be positive')
    # A mismatched procfs (for example a PID-virtualizing command sandbox)
    # cannot establish ownership. Refuse before spawning or signalling anything.
    proc_pid = int(Path('/proc/self/stat').read_text().split(' ', 1)[0])
    if proc_pid != os.getpid():
        raise RuntimeError('procfs PID namespace does not match this process; cannot supervise safely')
    _require_pidfds()
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    child = None
    owned = None
    status = 1
    received = []
    def interrupted(signum, _frame):
        received.append(signum)
    with log_path.open('wb', buffering=0) as log, _signals(interrupted):
        try:
            child = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=log, stderr=subprocess.STDOUT,
                                     close_fds=True, start_new_session=True)
            owned = OwnedProcesses(child)
            started = last_progress = time.monotonic()
            previous_cpu = {}
            previous_size = 0
            while True:
                if received:
                    raise Interrupted(received[0])
                live = owned.observe()
                now = time.monotonic()
                current_cpu = {}
                for pid, record in live.items():
                    own_allowed = cpu_progress_filter is None or cpu_progress_filter(pid)
                    waited_allowed = (own_allowed if waited_cpu_progress_filter is None
                                      else waited_cpu_progress_filter(pid))
                    if own_allowed:
                        current_cpu[(pid, record[0], 'self')] = record[4]
                    if waited_allowed:
                        current_cpu[(pid, record[0], 'waited')] = record[6]
                size = os.fstat(log.fileno()).st_size
                if (size != previous_size or any(ticks > previous_cpu.get(identity, 0)
                                                for identity, ticks in current_cpu.items())):
                    last_progress = now
                previous_cpu, previous_size = current_cpu, size
                result = child.poll()
                if result is not None:
                    status = result if result >= 0 else 128-result
                    break
                if now-started >= hard_timeout or now-last_progress >= inactivity_timeout:
                    reason = 'hard timeout' if now-started >= hard_timeout else 'inactivity timeout'
                    log.write(f'\n[supervisor] {reason}; terminating owned work\n'.encode())
                    status = 124
                    break
                time.sleep(poll_interval)
        except Interrupted as exc:
            status = 128+exc.signum
            log.write(f'\n[supervisor] interrupted by signal {exc.signum}\n'.encode())
        except OSError as exc:
            log.write(f'\n[supervisor] launch/monitor error: {exc}\n'.encode())
            status = 127
        finally:
            if child is not None:
                # Cover a signal arriving between Popen returning and identity capture.
                if owned is None:
                    owned = OwnedProcesses(child)
                remaining = owned.cleanup(term_grace, poll_interval)
                if remaining:
                    log.write(f'\n[supervisor] owned processes still present: {sorted(remaining)}\n'.encode())
                    if status == 0:
                        status = 1
    return 128+received[0] if received else status


def _fingerprint(path):
    if path is None:
        return None
    stat = path.stat()
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


def _artifact(root, config, name):
    candidates = sorted(p for p in (root/'x64'/config).glob(f'*/{name}') if p.is_file())
    direct = root/'x64'/config/name
    if direct.is_file():
        candidates.insert(0, direct)
    if len(candidates) > 1:
        raise RuntimeError(f'ambiguous build output {name}: {candidates}')
    return candidates[0] if candidates else None


def publish_pair(sources, output):
    """Prepare both files, then individually atomic promotions with rollback.

    The pair is not atomic to concurrent readers. A hard crash between replaces
    may leave a mixed pair; ordinary promotion errors roll back completed moves.
    """
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.build-stage-', dir=output) as directory:
        stage = Path(directory)
        backups = {}
        prepared = []
        for source in sources:
            target = output/source.name
            if target.exists() and not target.is_file():
                raise RuntimeError(f'output target is not a regular file: {target}')
            fresh = stage/(source.name+'.new')
            shutil.copy2(source, fresh)
            with fresh.open('rb') as stream:
                os.fsync(stream.fileno())
            backup = stage/(source.name+'.old')
            if target.exists():
                shutil.copy2(target, backup)
                backups[target] = backup
            else:
                backups[target] = None
            prepared.append((fresh, target))
        promoted = []
        try:
            for fresh, target in prepared:
                with _blocked_interrupts():
                    os.replace(fresh, target)
                    promoted.append(target)
        except BaseException:
            with _blocked_interrupts():
                for target in reversed(promoted):
                    backup = backups[target]
                    if backup is None:
                        target.unlink(missing_ok=True)
                    else:
                        os.replace(backup, target)
            raise



def _build_progress_names(pid):
    try:
        arguments = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return set()
    names = {argument.replace(b'\\', b'/').rsplit(b'/', 1)[-1].strip(b'"').lower()
             for argument in arguments}
    if names & {b'wineserver', b'wineserver64', b'winedevice.exe', b'services.exe', b'plugplay.exe'}:
        return set()
    return names


def _build_cpu_progress(pid):
    """Select workload CPU only AFTER run_step proves ownership.

    Executable names select progress contributors, never discover ownership.
    Owned Wine service CPU is not compiler progress.
    """
    return bool(_build_progress_names(pid) & {b'msbuild', b'msbuild.exe', b'cl.exe', b'link.exe'})


def _build_waited_cpu_progress(pid):
    # Credit reaped short-lived compiler workers to actual workloads. The
    # shell wrapper can also reap Wine infrastructure and must not contribute
    # that unclassified child CPU to compilation progress.
    return bool(_build_progress_names(pid) & {b'msbuild.exe', b'cl.exe', b'link.exe'})


def _build(root, config, extra):
    env = dict(os.environ)
    env['WINEPREFIX'] = env.get('WINEPREFIX') or str(Path.home()/'.local/opt/msvc-wineprefix')
    env['WINEDEBUG'] = env.get('WINEDEBUG') or '-all'
    compiler = Path(env.get('MSVC_BIN') or str(Path.home()/'.local/opt/msvc/bin/x64'))/'msbuild'
    date = datetime.now().strftime('%Y%m%d_%H%M%S')
    commit_result = subprocess.run(['git', '-C', str(root), 'rev-parse', '--short', 'HEAD'],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, close_fds=True)
    commit = commit_result.stdout.strip() if commit_result.returncode == 0 else 'unknown'
    changed = False
    for filename, value in (('resource_build_date.h', f'#define VER_BUILD_DATE "{date}"\n'),
                            ('resource_build_commit.h', f'#define VER_BUILD_COMMIT "{commit}"\n')):
        stamp = root/'OptiScaler'/filename
        old = stamp.read_text() if stamp.exists() else None
        if old != value:
            stamp.write_text(value)
            changed = True
    before = _fingerprint(_artifact(root, config, 'OptiScaler.dll'))
    logs = root/'x64/logs'/f'{date}-{os.getpid()}-{uuid.uuid4().hex[:8]}'
    logs.mkdir(parents=True)
    print(f'Build logs: {logs}', flush=True)
    windows_root = 'Z:'+str(root).replace('/', '\\')+'\\'
    common = ['/nologo', '/v:minimal', '/p:CL_MPCount=16', f'/p:Configuration={config}',
              '/p:Platform=x64', '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false',
              f'/p:SolutionDir={windows_root}']
    for label, project in (('forwarder', 'retoque/dlssnr/forwarder/dlssnr_forwarder.vcxproj'),
                           ('optiscaler', 'OptiScaler/OptiScaler.vcxproj')):
        log_path = logs/f'build-{label}.log'
        result = run_step([str(compiler), project, *common, *extra], cwd=root, env=env, log_path=log_path,
                          cpu_progress_filter=_build_cpu_progress,
                          waited_cpu_progress_filter=_build_waited_cpu_progress)
        with log_path.open('rb') as stream:
            shutil.copyfileobj(stream, sys.stdout.buffer)
        sys.stdout.flush()
        if result:
            print(f'{label} failed ({result}); evidence: {log_path}', file=sys.stderr)
            return result
    sources = [_artifact(root, config, name) for name in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll')]
    if any(source is None for source in sources):
        raise RuntimeError('compiler reported success but a required DLL is missing')
    if changed and _fingerprint(sources[0]) == before:
        raise RuntimeError('OptiScaler DLL is stale after build stamps changed')
    publish_pair(sources, root/'x64/out')
    for source in sources:
        print(f'x64/out/{source.name} <- {source}')
    return 0


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    config = (args[0] or 'Release') if args else 'Release'
    # Configuration is a directory name, not a path outside the checkout.
    if config in ('.', '..') or '/' in config or '\\' in config:
        print('Configuration must be a directory name', file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[1]
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')
    lock_path = runtime/'msvc-prefix.lock'
    try:
        with _signals(_interrupt), lock_path.open('a') as lock:
            os.set_inheritable(lock.fileno(), False)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:
                print(f'Compiler prefix lock busy: {lock_path}', file=sys.stderr)
                return 75
            return _build(root, config, args[1:])
    except Interrupted as exc:
        return 128+exc.signum
    except (OSError, RuntimeError) as exc:
        print(f'Build failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
