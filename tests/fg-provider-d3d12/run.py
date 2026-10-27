#!/usr/bin/env python3
"""Full production OptiScaler + actual local DLSS-G plugin. No game and no substitute backend."""
import argparse
import configparser
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from _suite_result import result_run, write_result
OUT = HERE / 'artifacts'
WORK = OUT / 'run'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
DLLS = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def guard():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        comm = ''
        try:
            comm = (proc / 'comm').read_text().strip().lower()
            if comm in ('msbuild.exe', 'cl.exe', 'link.exe'):
                raise RuntimeError('compiler prefix already in use')
            mappings = (proc / 'maps').read_text()
            if str(WORK) in mappings:
                raise RuntimeError('an earlier provider fixture still maps its staged DLLs')
            if '/steamapps/common/' in mappings:
                raise RuntimeError('a Steam common application is mapped; close it before the GPU fixture')
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            if proc.exists() and proc.stat().st_uid == os.getuid() and comm not in ('systemd', '(sd-pam)'):
                raise RuntimeError('cannot inspect an owned process for active game mappings')


def execute(command, *, cwd, env, log, timeout):
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        raise RuntimeError('process supervision requires default SIGCHLD')
    print('+', ' '.join(map(str, command)), flush=True)
    with log.open('w') as stream:
        child = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return child.wait(timeout=timeout)
        except BaseException:
            # A completed/reaped leader no longer reserves the group's numeric ID.
            if child.returncode is not None:
                raise
            # With default SIGCHLD and this fixture as sole reaper, retain that
            # reservation through TERM/grace/KILL. Never poll/wait in the grace.
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                time.sleep(3)
            finally:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
            raise


def stage(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + '.stage')
    temporary.unlink(missing_ok=True)
    shutil.copy2(source, temporary)
    temporary.replace(target)  # Never truncate a DLL that a process may still have mapped.


def judge(text):
    phases = re.findall(r'^PHASE phase=(\d+) enabled=(\d+) app=(\d+) actual=(\d+) '
                        r'steady_app=(\d+) steady_actual=(\d+) ok=(\d+) completed=(\d+)$', text, re.M)
    if len(phases) != 4 or 'ORACLE=real-output-plugin' not in text:
        raise RuntimeError('ZERO COVERAGE: complete real-plugin phase record missing')
    frames = [tuple(map(int, row)) for row in re.findall(
        r'^FRAME phase=(\d+) app=(\d+) actual=(\d+) status=(\d+) max=(\d+)$', text, re.M)]
    if len(frames) != 336 or [row[1] for row in frames] != list(range(1, 337)):
        raise RuntimeError('incomplete or non-monotonic per-frame progression')
    result = []
    for index, row in enumerate(phases):
        phase, enabled, app, actual, steady_app, steady_actual, ok, completed = map(int, row)
        expected = 48 if index == 1 else 96
        if phase != index or enabled != (index != 1) or app != expected or completed != expected:
            raise RuntimeError('incomplete frame progression or completion')
        if steady_app != expected - 32 or ok != steady_app:
            raise RuntimeError('missing steady-state successful plugin status')
        if (enabled and steady_actual < steady_app + 16) or (not enabled and steady_actual != steady_app):
            raise RuntimeError('plugin actual-present surplus disagrees with enable state')
        observed = [row for row in frames if row[0] == phase]
        if (len(observed) != app or sum(row[2] for row in observed) != actual
                or sum(row[2] for row in observed[32:]) != steady_actual
                or any(row[3] != 0 for row in observed[32:])
                or any(row[4] < 1 for row in observed if enabled)):
            raise RuntimeError('phase counters disagree with actual per-frame plugin observations')
        result.append(dict(phase=phase, enabled=bool(enabled), app=app, actual=actual,
                           steady_app=steady_app, steady_actual=steady_actual, completed=completed))
    total_app, total_actual = sum(x['app'] for x in result), sum(x['actual'] for x in result)
    marker = f'PASS: FG-PROVIDER app={total_app} actual={total_actual} phases=4 resizes=1 oracle=real-plugin'
    if marker not in text or len(re.findall(r'^FRAME ', text, re.M)) != total_app:
        raise RuntimeError('incomplete frame records or final marker')
    return result


def build_ini(multiplier=2, *, ada_mfg_unlock=False):
    if multiplier not in (2, 3, 4):
        raise ValueError('multiplier must be 2, 3, or 4')
    return ('[FrameGen]\nEnabled=auto\nFGInput=dlssg\nFGOutput=dlssg\nFGNvngxReplacement=None\n'
            f'[DLSSG]\nInterpolationCount={multiplier-1}\nOverrideInterpolationCount=auto\n'
            f'AdaMfgUnlock={str(ada_mfg_unlock).lower()}\nForceDMFG=false\nOverrideForceDMFG=false\n'
            '[Menu]\nOverlayMenu=false\n[DlssNr]\nEnabled=false\n[Vulkan]\nSkipHooks=true\n'
            '[Log]\nLogToFile=true\nLogLevel=1\n'
            '[Spoofing]\nDxgi=false\nVulkan=false\nStreamlineSpoofing=false\n')


def verify_staged_provenance(before, after, launch_ini, final_ini):
    """Keep binaries immutable; account for the production INI writer's known output."""
    if set(before) != set(after):
        raise RuntimeError('staged runtime artifact set changed; provenance invalid')
    changed = sorted(name for name in before if before[name] != after[name] and name != 'OptiScaler.ini')
    if changed:
        raise RuntimeError('staged runtime binaries changed: ' + ', '.join(changed))
    launch_hash, final_hash = (hashlib.sha256(data).hexdigest() for data in (launch_ini, final_ini))
    if before.get('OptiScaler.ini') != launch_hash or after.get('OptiScaler.ini') != final_hash:
        raise RuntimeError('configuration snapshot hashes disagree; provenance invalid')

    def settings(data):
        config = configparser.ConfigParser(interpolation=None, strict=True)
        try:
            config.read_string(data.decode('utf-8-sig'))
        except (UnicodeError, configparser.Error) as error:
            raise RuntimeError('invalid runtime INI: ' + str(error)) from error
        if config.defaults():
            raise RuntimeError('unexpected INI defaults; provenance invalid')
        result, sections = {}, set()
        for section in config.sections():
            normalized = section.casefold()
            if normalized in sections:
                raise RuntimeError('duplicate INI section identity; provenance invalid')
            sections.add(normalized)
            for key, value in config[section].items():
                result[(normalized, key.casefold())] = value.strip()
        return result

    requested, observed = settings(launch_ini), settings(final_ini)
    if any(observed.get(key) != value for key, value in requested.items()):
        raise RuntimeError('explicit fixture configuration changed during execution')
    # The production Streamline input persists these camera flags for XeFG even
    # with DLSS-G selected. Permit only this fixture's exact observed values.
    allowed = {('xefg', 'depthinverted'): 'false', ('xefg', 'jitteredmv'): 'auto',
               ('xefg', 'highresmv'): 'true'}
    added = {key: value for key, value in observed.items() if key not in requested}
    if any(allowed.get(key) != value for key, value in added.items()):
        raise RuntimeError('unexpected runtime configuration additions')
    return {'launch_sha256': launch_hash, 'final_sha256': final_hash,
            'rewritten': launch_hash != final_hash, 'explicit_settings_unchanged': True,
            'runtime_added_settings': {'.'.join(key): value for key, value in sorted(added.items())}}


def assess(text, multiplier=2):
    """Keep the x2 runtime gate; qualify only complete, exact counter windows.

    The SDK defines actual as presents since the preceding GetState call. Samples
    delimit whole call intervals, so no interval is split or interpolated. Five
    seconds is the existing moving-window method, not a tolerance on the ratio.
    This qualifies plugin counter ratios in this fixture, never display scanout,
    unique generated images, latency, or general support of a hardware family.
    """
    phases = judge(text)
    requested = re.findall(r'^REQUEST multiplier=(\d+) generated=(\d+)$', text, re.M)
    if (multiplier not in (2, 3, 4) or requested != [(str(multiplier), str(multiplier-1))]
            or len(re.findall(r'^REQUEST ', text, re.M)) != 1):
        raise RuntimeError('missing or conflicting requested multiplier record')
    frames = [tuple(map(int, row)) for row in re.findall(
        r'^FRAME phase=(\d+) app=(\d+) actual=(\d+) status=(\d+) max=(\d+)$', text, re.M)]
    samples = [tuple(map(int, row)) for row in re.findall(
        r'^SAMPLE phase=(\d+) app=(\d+) us=(\d+)$', text, re.M)]
    baselines = [tuple(map(int, row)) for row in re.findall(
        r'^BASELINE phase=(\d+) us=(\d+)$', text, re.M)]
    if len(samples) != len(re.findall(r'^SAMPLE ', text, re.M)):
        raise RuntimeError('malformed timing record')
    if len(baselines) != len(re.findall(r'^BASELINE ', text, re.M)):
        raise RuntimeError('malformed timing baseline')
    if samples and [(p, a) for p, a, _ in samples] != [(r[0], r[1]) for r in frames]:
        raise RuntimeError('incomplete, duplicate, or reordered per-call timing records')
    if baselines and [p for p, _ in baselines] != list(range(4)):
        raise RuntimeError('incomplete or duplicate timing baselines')
    times = {(p, a): us for p, a, us in samples}
    baseline = dict(baselines)
    gpu = re.findall(r'^GPU vendor=(0x[0-9a-fA-F]+) device=(0x[0-9a-fA-F]+) '
                     r'subsystem=(0x[0-9a-fA-F]+) revision=(\d+) '
                     r'luid=([0-9a-fA-F]+:[0-9a-fA-F]+) description=(.+)$', text, re.M)
    modules = re.findall(r'^MODULE role=(output_plugin|provider) path=(.+)$', text, re.M)
    module_paths = dict(modules)
    reasons, windows, observed, caps = [], [], [], []
    if len(gpu) != 1 or int(gpu[0][0], 16) != 0x10de:
        reasons.append('selected NVIDIA adapter identity is missing or ambiguous')
    if (len(modules) != 2 or set(module_paths) != {'output_plugin', 'provider'} or
            any(re.match(r'^[A-Za-z]:[\\/]', value) is None for value in module_paths.values())):
        reasons.append('actual output-plugin/provider module paths are missing or ambiguous')
    if not samples or not baselines:
        reasons.append('per-call timing or phase baselines are missing')
    previous_phase_end = None
    for phase in phases:
        index = phase['phase']
        rows = [r for r in frames if r[0] == index]
        if samples and baselines:
            last = baseline[index]
            if previous_phase_end is not None and last < previous_phase_end:
                raise RuntimeError('phase timing moved backwards')
            for row in rows:
                now = times[(index, row[1])]
                if now <= last:
                    raise RuntimeError('non-monotonic per-call timing')
                last = now
            previous_phase_end = last
        if not phase['enabled']:
            continue
        steady = rows[32:]
        caps.extend(r[4] for r in steady)
        observed.append({'phase': index, 'application_presents': phase['steady_app'],
                         'plugin_actual_presents': phase['steady_actual'],
                         'ratio': phase['steady_actual']/phase['steady_app']})
        if not samples or not baselines:
            continue
        start = times[(index, rows[31][1])]
        app, actual, phase_windows = 0, 0, 0
        for row in steady:
            app += 1
            actual += row[2]
            end = times[(index, row[1])]
            if end-start >= 5_000_000:
                windows.append({'phase': index, 'start_us': start, 'end_us': end,
                                'application_presents': app, 'plugin_actual_presents': actual,
                                'ratio': actual/app, 'requested_ratio_observed': actual == app*multiplier})
                phase_windows += 1
                if actual != app*multiplier:
                    reasons.append(f'phase {index} complete counter window differs from requested x{multiplier}')
                start, app, actual = end, 0, 0
        if phase_windows == 0:
            reasons.append(f'phase {index} has no complete five-second steady window')
    if min(caps) < multiplier-1:
        reasons.append('provider-reported steady cap is below the requested generated count')
    qualification = {'status': 'INCONCLUSIVE' if reasons else 'PASS', 'reasons': reasons,
                     'scope': 'plugin-present ratio in this fixture; no hardware-family approval',
                     'window_method': 'whole GetState intervals spanning at least five seconds; exact integer ratio',
                     'complete_windows': windows, 'partial_tails': 'excluded from window qualification'}
    return {'status': 'PASS' if multiplier == 2 or not reasons else 'INCONCLUSIVE',
            'reason': '; '.join(reasons) if multiplier != 2 else '',
            'runtime_regression': 'PASS', 'requested_multiplier': multiplier,
            'requested_generated_frames': multiplier-1,
            'effective_configured_generated_frames': None,
            'effective_configured_reason': 'GetState options parameter is input, not applied-option readback',
            'observed_steady_ratios': observed,
            'provider_capability': {'scope': 'real output-plugin response in this run, including configured policies',
                                    'steady_min_generated_frames': min(caps),
                                    'steady_max_generated_frames': max(caps)},
            'qualification': qualification, 'phases': phases,
            'gpu': (dict(zip(('vendor', 'device', 'subsystem', 'revision', 'luid', 'description'), gpu[0]))
                    if len(gpu) == 1 else None),
            'loaded_module_paths': module_paths}


def runtime_identity(proton, module_paths, compat):
    """Read-only host/selected-runtime identity; no GPU discovery or device mutation."""
    cpu = {'source': '/proc/cpuinfo', 'model': None, 'logical_processors': os.cpu_count()}
    try:
        first = Path('/proc/cpuinfo').read_text().split('\n\n', 1)[0]
        fields = dict(line.split(':', 1) for line in first.splitlines() if ':' in line)
        fields = {key.strip(): value.strip() for key, value in fields.items()}
        cpu.update(model=fields.get('model name', fields.get('Hardware')),
                   vendor=fields.get('vendor_id'), family=fields.get('cpu family'),
                   model_id=fields.get('model'), stepping=fields.get('stepping'))
    except OSError as error:
        cpu['unavailable'] = str(error)
    def file_record(path):
        try:
            return {'path': str(path), 'sha256': sha(path)}
        except OSError as error:
            return {'path': str(path), 'sha256': None, 'unavailable': str(error)}
    def text_record(path):
        if not path.is_file():
            return None
        record = file_record(path)
        try:
            record['text'] = path.read_text(errors='replace').strip()
        except OSError as error:
            record['unavailable'] = str(error)
        return record
    modules = {}
    for role, windows_path in module_paths.items():
        normalized = windows_path.replace('\\', '/')
        if normalized[:3].lower() == 'z:/':
            path = Path(normalized[2:])
        elif normalized[:3].lower() == 'c:/':
            path = compat/'pfx/drive_c'/normalized[3:]
        else:
            modules[role] = {'windows_path': windows_path, 'sha256': None,
                             'unavailable': 'module drive mapping is unknown'}
            continue
        modules[role] = {**file_record(path), 'windows_path': windows_path}
    version = proton.parent/'version'
    driver = Path('/proc/driver/nvidia/version')
    return {'system': {'platform': platform.platform(), 'machine': platform.machine(),
                       'kernel': platform.release(), 'python': platform.python_version()},
            'cpu': cpu, 'proton_launcher': file_record(proton),
            'proton_version_file': text_record(version),
            'linux_nvidia_driver': text_record(driver),
            'loaded_module_files': modules,
            'scope': 'host identity, selected launcher, and module-file hashes read after execution; not a hash of mapped memory'}


def finalize_assessment(assessment, identity):
    """Counter evidence alone cannot qualify an unidentified runtime/driver."""
    gaps = []
    if not (identity.get('cpu') or {}).get('model'):
        gaps.append('CPU identity is unavailable')
    if not (identity.get('proton_launcher') or {}).get('sha256'):
        gaps.append('selected Proton launcher hash is unavailable')
    for key, label in (('proton_version_file', 'Proton runtime version'),
                       ('linux_nvidia_driver', 'NVIDIA driver version')):
        record = identity.get(key) or {}
        if not isinstance(record.get('text'), str) or not record['text'].strip() or not record.get('sha256'):
            gaps.append(f'{label} and version-file hash are unavailable')
    modules = identity.get('loaded_module_files') or {}
    if (set(modules) != {'provider', 'output_plugin'} or
            any(not record.get('sha256') for record in modules.values())):
        gaps.append('loaded-module file identity is incomplete')
    if gaps:
        assessment['qualification']['status'] = 'INCONCLUSIVE'
        assessment['qualification']['reasons'].extend(gaps)
        if assessment['requested_multiplier'] != 2:
            assessment['status'] = 'INCONCLUSIVE'
            assessment['reason'] = '; '.join(assessment['qualification']['reasons'])
    return assessment


def cleanup_self_test():
    with tempfile.TemporaryDirectory(prefix='retoque-fg-timeout-') as temporary:
        folder = Path(temporary)
        ready = folder / 'ready'
        child_code = ('import os,signal,time;from pathlib import Path;'
                      'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
                      'stat=Path("/proc/self/stat").read_text().rsplit(") ",1)[1].split();'
                      f'Path({str(ready)!r}).write_text(str(os.getpid())+":"+stat[19]);time.sleep(60)')
        parent_code = ('import subprocess,sys,time;from pathlib import Path;'
                       f'p=subprocess.Popen([sys.executable,"-c",{child_code!r}]);'
                       f'ready=Path({str(ready)!r});'
                       '\nwhile not ready.exists(): time.sleep(.01)'
                       '\ntime.sleep(60)')
        descendant = None
        descendant_start = None
        try:
            try:
                execute([sys.executable, '-c', parent_code], cwd=folder, env=dict(os.environ),
                        log=folder / 'log', timeout=1)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise RuntimeError('timeout cleanup negative control did not time out')
            if not ready.is_file():
                raise RuntimeError('timeout cleanup child never became ready')
            descendant, descendant_start = map(int, ready.read_text().split(':'))
            stat = Path(f'/proc/{descendant}/stat')
            for _ in range(100):
                try:
                    current = stat.read_text().rsplit(') ', 1)[1].split()
                except FileNotFoundError:
                    break
                if current[0] == 'Z' or int(current[19]) != descendant_start:
                    break
                time.sleep(.01)
            else:
                raise RuntimeError('timeout left SIGTERM-ignoring descendant alive')
        finally:
            if descendant is None and ready.is_file():
                descendant, descendant_start = map(int, ready.read_text().split(':'))
            if descendant is not None:
                try:
                    descriptor = os.pidfd_open(descendant)
                    try:
                        current = Path(f'/proc/{descendant}/stat').read_text().rsplit(') ', 1)[1].split()
                        if current[0] != 'Z' and int(current[19]) == descendant_start:
                            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                    finally:
                        os.close(descriptor)
                except (ProcessLookupError, FileNotFoundError):
                    pass
    print('PASS: wrapper timeout kills SIGTERM-ignoring descendant (host only)')


def self_test():
    rows = ['ORACLE=real-output-plugin']
    actual_sum = 0
    frame_id = 0
    for phase in range(4):
        count = 48 if phase == 1 else 96
        actual = count if phase == 1 else count * 2
        steady = count - 32
        for _ in range(count):
            frame_id += 1
            rows.append(f'FRAME phase={phase} app={frame_id} actual={1 if phase == 1 else 2} status=0 max=1')
        rows.append(f'PHASE phase={phase} enabled={int(phase != 1)} app={count} actual={actual} '
                    f'steady_app={steady} steady_actual={steady if phase == 1 else steady * 2} '
                    f'ok={steady} completed={count}')
        actual_sum += actual
    rows.append(f'PASS: FG-PROVIDER app=336 actual={actual_sum} phases=4 resizes=1 oracle=real-plugin')
    valid = '\n'.join(rows)
    judge(valid)
    for invalid in (valid.replace('steady_actual=128', 'steady_actual=64', 1),
                    valid.replace('ORACLE=real-output-plugin', ''),
                    valid.replace('completed=96', 'completed=95', 1),
                    valid.replace('enabled=0', 'enabled=1', 1),
                    valid.replace('steady_actual=16', 'steady_actual=0', 1), valid.replace('FRAME phase=0 app=1 actual=2 status=0 max=1\n', '', 1)):
        try:
            judge(invalid)
        except RuntimeError:
            pass
        else:
            raise RuntimeError('judge accepted a broken evidence record')
    print('PASS: FG provider evidence judge and six negative controls (host only)')
    subprocess.run([sys.executable, str(HERE/'test_qualification.py')], check=True)
    cleanup_self_test()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, default=ROOT / 'x64/out/OptiScaler.dll')
    parser.add_argument('--kit', type=Path, default=Path(os.environ['RETOQUE_FG_KIT']) if 'RETOQUE_FG_KIT' in os.environ else None)
    parser.add_argument('--proton', type=Path, default=Path(os.environ['RETOQUE_PROTON']) if 'RETOQUE_PROTON' in os.environ else None)
    parser.add_argument('--dlss', type=Path, help='existing nvngx_dlss.dll required by native NGX routing')
    parser.add_argument('--strict', action='store_true', help='missing prerequisites fail instead of SKIP')
    parser.add_argument('--multiplier', type=int, choices=(2, 3, 4), default=2,
                        help='requested total multiplier; x3/x4 require counter qualification for top-level PASS')
    parser.add_argument('--ada-mfg-unlock', action='store_true',
                        help='explicitly enable the existing Ada memory unlock only in the staged fixture INI')
    parser.add_argument('--self-test', action='store_true', help='exercise only the host evidence judge')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    OUT.mkdir(parents=True, exist_ok=True)
    write_result(OUT / 'result.json', {'status': 'RUNNING'})
    steam = Path.home() / '.local/share/Steam'
    kit = args.kit
    if kit is None:
        candidates = sorted((Path.home() / '.local/state/dlss5/rafael-kits').glob('*/streamline'), reverse=True)
        kit = next((p for p in candidates if all((p / name).is_file() for name in DLLS)), None)
    proton = args.proton
    if proton is None:
        candidates = sorted((steam / 'compatibilitytools.d').glob('GE-Proton*/proton'), reverse=True)
        candidates += [steam / 'steamapps/common/Proton - Experimental/proton']
        proton = next((p for p in candidates if p.is_file()), None)
    dlss = args.dlss
    if dlss is None:
        dlss = next(iter(sorted((Path.home() / '.local/state/dlss5/rafael-kits').glob('*/nvngx_dlss.dll'), reverse=True)), None)
    missing = []
    if dlss is None or not dlss.is_file():
        missing.append('existing nvngx_dlss.dll for native NGX routing (--dlss)')
    if not (MSVC / 'cl').is_file():
        missing.append('MSVC cl wrapper (MSVC_BIN)')
    if not PREFIX.is_dir():
        missing.append('compiler prefix (WINEPREFIX)')
    if not args.dll.is_file():
        missing.append('production OptiScaler.dll (--dll, build-local.sh first)')
    if kit is None or any(not (kit / name).is_file() for name in DLLS):
        missing.append('complete user-supplied Streamline/DLSS-G kit (--kit / RETOQUE_FG_KIT)')
    if proton is None or not proton.is_file():
        missing.append('installed Proton executable (--proton / RETOQUE_PROTON)')
    if not os.environ.get('DISPLAY') and not os.environ.get('WAYLAND_DISPLAY'):
        missing.append('graphical session')
    if missing:
        result = {'status': 'FAIL' if args.strict else 'SKIP', 'missing': missing, 'runtime_coverage': False}
        write_result(OUT / 'result.json', result)
        print(result['status'] + ': ' + '; '.join(missing))
        if args.strict:
            raise SystemExit(1)
        return
    lock_path = Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}') / 'msvc-prefix.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('compiler prefix locked by another task')
        guard()
        WORK.mkdir(parents=True, exist_ok=True)
        stage(args.dll, WORK / 'OptiScaler.dll')
        stage(dlss, WORK / 'nvngx_dlss.dll')
        for name in DLLS:
            stage(kit / name, WORK / 'streamline' / name)
            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll'):
                stage(kit / name, WORK / 'input' / name)
        redists = sorted((MSVC.parents[1] / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'))
        if not redists:
            raise RuntimeError('MSVC runtime missing')
        for path in redists[-1].glob('*.dll'):
            stage(path, WORK / path.name)
        launch_ini = build_ini(args.multiplier, ada_mfg_unlock=args.ada_mfg_unlock).encode('utf-8')
        launch_snapshot = OUT / 'OptiScaler.launch.ini'
        launch_snapshot.write_bytes(launch_ini)
        (WORK / 'OptiScaler.ini').write_bytes(launch_ini)
        for name in ('fg-provider.log', 'OptiScaler.log'):
            (WORK / name).unlink(missing_ok=True)
        source_hashes = {path.name: sha(path) for path in (HERE / 'harness.cpp', HERE / 'run.py')}
        compile_env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
        exe = WORK / 'fg-provider.exe'
        command = [MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
                   '/I' + str(ROOT / 'external/streamline'), HERE / 'harness.cpp',
                   '/Fo' + str(OUT / 'harness.obj'), '/Fe' + str(exe), '/link',
                   'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
        code = execute(command, cwd=ROOT, env=compile_env, log=OUT / 'build.log', timeout=300)
        if code:
            raise RuntimeError('fixture compile failed; see artifacts/build.log')
        staged_assets = {'OptiScaler.dll': sha(WORK / 'OptiScaler.dll'),
                         'OptiScaler.ini': sha(WORK / 'OptiScaler.ini'), 'fg-provider.exe': sha(exe),
                         'nvngx_dlss.dll': sha(WORK / 'nvngx_dlss.dll'),
                         **{name: sha(WORK / 'streamline' / name) for name in DLLS},
                         **{'input/' + name: sha(WORK / 'input' / name) for name in DLLS
                            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll')}}
        guard()
        compat = OUT / 'protonprefix'
        compat.mkdir(exist_ok=True)
        env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(compat),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), SteamGameId='0',
                   WINEDEBUG='-all', PROTONFIXES_DISABLE='1',
                   PROTON_FSR4_UPGRADE='0', PROTON_FFX3_UPGRADE='0', PROTON_FFX4_UPGRADE='0',
                   PROTON_DLSS_UPGRADE='0', PROTON_XESS_UPGRADE='0', PROTON_USE_OPTISCALER='0', PROTON_LOG='1', PROTON_LOG_DIR=str(OUT),
                   VK_LOADER_LAYERS_DISABLE='~implicit~',
                   WINEDLLOVERRIDES='vcruntime140,vcruntime140_1,msvcp140=n')
        launcher_hash = sha(proton)
        code = execute([proton, 'run', exe, str(args.multiplier)], cwd=WORK, env=env, log=OUT / 'proton.log', timeout=240)
        text = (WORK / 'fg-provider.log').read_text(errors='replace') if (WORK / 'fg-provider.log').exists() else ''
        if code:
            raise RuntimeError(f'provider exited {code}; see artifacts/run/fg-provider.log and proton.log')
        assessment = assess(text, args.multiplier)
        after_assets = {'OptiScaler.dll': sha(WORK / 'OptiScaler.dll'),
                         'OptiScaler.ini': sha(WORK / 'OptiScaler.ini'), 'fg-provider.exe': sha(exe),
                        'nvngx_dlss.dll': sha(WORK / 'nvngx_dlss.dll'),
                        **{name: sha(WORK / 'streamline' / name) for name in DLLS},
                         **{'input/' + name: sha(WORK / 'input' / name) for name in DLLS
                            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll')}}
        if source_hashes != {path.name: sha(path) for path in (HERE / 'harness.cpp', HERE / 'run.py')}:
            raise RuntimeError('fixture sources changed during build/run; provenance invalid')
        if sha(launch_snapshot) != staged_assets['OptiScaler.ini']:
            raise RuntimeError('launch configuration snapshot changed; provenance invalid')
        config_provenance = verify_staged_provenance(
            staged_assets, after_assets, launch_ini, (WORK / 'OptiScaler.ini').read_bytes())
        config_provenance['launch_snapshot'] = str(launch_snapshot)
        if sha(proton) != launcher_hash:
            raise RuntimeError('selected Proton launcher changed during execution; provenance invalid')
        opti = (WORK / 'OptiScaler.log').read_text(errors='replace')
        if 'DLSSG_Dx12::Dispatch' not in opti or 'Result: Ok' not in opti:
            raise RuntimeError('ZERO COVERAGE: production DLSS-G dispatch evidence missing')
        identity = runtime_identity(proton, assessment['loaded_module_paths'], compat)
        assessment = finalize_assessment(assessment, identity)
        result = {**assessment, 'runtime_coverage': True,
                  'runtime_identity': identity, 'ada_mfg_unlock_requested': args.ada_mfg_unlock,
                   'ini_sha256': staged_assets['OptiScaler.ini'], 'ini_final_sha256': after_assets['OptiScaler.ini'],
                   'config_provenance': config_provenance, 'harness_sha256': staged_assets['fg-provider.exe'],
                  'dll_sha256': staged_assets['OptiScaler.dll'], 'dlss_sha256': staged_assets['nvngx_dlss.dll'],
                   'assets': {name: staged_assets[name] for name in DLLS},
                   'staged_assets_unchanged': staged_assets == after_assets, 'staged_binaries_unchanged': True,
                  'sources': source_hashes,
                  'input_assets': {name: value for name, value in staged_assets.items() if name.startswith('input/')},
                  'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
                  'hardware_matrix': {'status': 'PENDING', 'approval_issued': False,
                                      'targets': [{'gpu': 'RTX 4090', 'runtime': 'Proton', 'multipliers': [2, 3, 4], 'status': 'PENDING_REVIEW'},
                                                  {'gpu': 'RTX 3060', 'runtime': 'native Windows', 'multipliers': [2, 3, 4], 'status': 'NOT_RUN'}]},
                  'cases_not_exercised': {'other_multipliers': [x for x in (2, 3, 4) if x != args.multiplier],
                                         'native_windows': 'NOT_RUN: fixture launcher uses Proton',
                                         'menu': 'SKIP: no synthetic menu input in this fixture'},
                  'limits': ['no hardware-family approval', 'plugin actual-present counters, not scanout capture or unique generated-image proof',
                             'no perceptual-quality or latency metric', 'Proton is not native D3D12 validation',
                             'no Vulkan/D3D11/menu coverage']}
        write_result(OUT / 'result.json', result)
        print(f"{result['status']}: provider runtime PASS; requested x{args.multiplier}; "
              f"counter qualification {result['qualification']['status']}; {OUT / 'result.json'}")


if __name__ == '__main__':
    try:
        with result_run(OUT / 'result.json'):
            main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        OUT.mkdir(parents=True, exist_ok=True)
        write_result(OUT / 'result.json', {'status': 'FAIL', 'runtime_coverage': False,
                                          'reason': str(error)})
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
