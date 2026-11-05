#!/usr/bin/env python3
"""Isolated transport experiment. No game, NGX, production DLL or shared-prefix mutation."""
from __future__ import annotations
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
sys.path.insert(0, str(ROOT / 'tests'))
from _suite_result import result_run, write_result


def judge(result, code, mutant=False, cycle=False):
    if cycle:
        return 'PASS' if (code == 3 and result.get('status') == 'STALL'
                          and 0 < result.get('P', 0) < (1 << 64) - 1
                          and all(result.get(key) == 0 for key in ('N', 'C', 'B'))) else 'FAIL'
    if result.get('status') == 'STALL' and code == 3:
        return 'INCONCLUSIVE'
    if code == 0 and result.get('status') == 'SKIP' and result.get('reason'):
        return 'SKIP'
    covered = result.get('frames') == 24 and result.get('verified') == 24
    if mutant:
        return 'PASS' if (code == 1 and covered and result.get('status') == 'FAIL'
                          and result.get('reason') == 'pixel mismatch'
                          and result.get('mismatches', 0) > 0) else 'FAIL'
    return 'PASS' if code == 0 and covered and result.get('status') == 'PASS' and result.get('mismatches', 0) == 0 else 'FAIL'


def guide_helpers():
    spec = importlib.util.spec_from_file_location('guide_runner', ROOT / 'tests/dx11-guides-transport/run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prerequisite(helper):
    if os.environ.get('PROTON_DIR') and not (Path(os.environ['PROTON_DIR']) / 'proton').is_file():
        return None, 'explicit PROTON_DIR has no Proton executable'
    runtime = helper.proton()
    if not runtime:
        return None, 'no local Proton'
    if not (runtime.parent / 'files/bin/wineserver').is_file():
        return None, 'selected Proton has no fixture-cleanup wineserver'
    for executable in ('wine', 'wineserver', 'cp'):
        if not shutil.which(executable):
            return None, f'missing {executable}'
    if not (helper.MSVC / 'cl').is_file() or not helper.TOOLCHAIN_PREFIX.is_dir():
        return None, 'missing MSVC toolchain or template prefix'
    if not (ROOT / 'external/vulkan/include/vulkan/vulkan_core.h').is_file():
        return None, 'missing vendored Vulkan headers for read-only queue identity query'
    if not os.environ.get('DISPLAY'):
        return None, 'no DISPLAY for DXGI Present fixture'
    try:
        helper.no_game()
        helper.refuse_if_prefix_busy()
    except RuntimeError as error:
        return None, str(error).split('\n')[0]
    return runtime, None


def assert_owned_prefix(prefix):
    allowed = {OUT / 'build-prefix', OUT / 'protonprefix/pfx'}
    if prefix not in allowed or prefix.resolve() not in allowed:
        raise RuntimeError('refusing a foreign or symlinked prefix')


def stop_prefix(server, prefix):
    # Only prefixes created by this fixture; never kill a system/shared Wine server.
    assert_owned_prefix(prefix)
    subprocess.run([str(server), '-k'], env=dict(os.environ, WINEPREFIX=str(prefix)),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False)


def command(args, log, *, cwd, env, timeout):
    with log.open('w') as output:
        process = subprocess.Popen(args, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            return process.wait(timeout=timeout)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def build(helper):
    prefix = OUT / 'build-prefix'
    assert_owned_prefix(prefix)
    if not prefix.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(helper.TOOLCHAIN_PREFIX.resolve()), str(prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    stop_prefix('wineserver', prefix)
    try:
        code = command([str(helper.MSVC / 'cl'), '/nologo', '/std:c++20', '/EHsc', '/MT', '/W4', '/WX', '/O1',
                        '/I' + str(ROOT / 'external/vulkan/include'), str(HERE / 'harness.cpp'), '/Fo' + str(OUT) + '/', '/Fe' + str(OUT / 'transport.exe'),
                        '/link', 'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'd3dcompiler.lib', 'user32.lib'],
                       OUT / 'build.log', cwd=OUT, env=env, timeout=300)
    finally:
        stop_prefix('wineserver', prefix)
    if code:
        raise RuntimeError('MSVC build failed: see artifacts/build.log')


def identity(runtime):
    files = {}
    for pattern in ('files/bin/wine', 'files/bin/wine64', 'files/bin/wineserver',
                    'files/lib64/wine/dxvk/*.dll', 'files/lib64/wine/vkd3d-proton/*.dll',
                    'files/lib/wine/dxvk/x86_64-windows/*.dll',
                    'files/lib/wine/vkd3d-proton/x86_64-windows/*.dll'):
        for path in sorted(runtime.parent.glob(pattern)):
            files[str(path.relative_to(runtime.parent))] = sha(path)
    versions = {}
    for name in ('version', 'toolmanifest.vdf'):
        path = runtime.parent / name
        if path.is_file():
            versions[name] = path.read_text(errors='replace')
    gpu = subprocess.run(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader'],
                         capture_output=True, text=True, check=False) if shutil.which('nvidia-smi') else None
    controls = {key: os.environ[key] for key in ('VKD3D_CONFIG', 'VKD3D_FEATURE_LEVEL', 'DXVK_CONFIG',
                'DXVK_FILTER_DEVICE_NAME') if key in os.environ}
    return dict(proton=runtime.parent.name, versions=versions, files=files, inherited_controls=controls,
                gpu=gpu.stdout.strip() if gpu else 'not queried', executable_sha256=sha(OUT / 'transport.exe'))


def run_case(runtime, mode):
    work = OUT / mode
    work.mkdir(exist_ok=True)
    for name in ('case.json', 'trace.log', 'transport_d3d11.log', 'transport_dxgi.log'):
        (work / name).unlink(missing_ok=True)
    compat = OUT / 'protonprefix'
    prefix = compat / 'pfx'
    assert_owned_prefix(prefix)
    compat.mkdir(exist_ok=True)
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(compat),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / '.local/share/Steam'), SteamGameId='0',
               WINEDEBUG='-all', PYTHONDONTWRITEBYTECODE='1', DXVK_LOG_LEVEL='info', DXVK_LOG_PATH=str(work), VKD3D_DEBUG='info')
    env.pop('WINEPREFIX', None)
    start = time.monotonic()
    code = None
    timed_out = False
    try:
        code = command([str(runtime), 'run', str(OUT / 'transport.exe'), mode], work / 'runtime.log',
                       cwd=work, env=env, timeout=60)
    except subprocess.TimeoutExpired:
        timed_out = True
    finally:
        # Proton's matching server and this fixture's private compat prefix only.
        server = runtime.parent / 'files/bin/wineserver'
        if prefix.exists() and server.is_file():
            stop_prefix(server, prefix)
    case = json.loads((work / 'case.json').read_text()) if (work / 'case.json').exists() else {}
    trace = (work / 'trace.log').read_text(errors='replace') if (work / 'trace.log').exists() else ''
    logs = '\n'.join(p.read_text(errors='replace') for p in work.glob('*.log'))
    status = judge(case, code, mutant=mode in ('mutant-copy', 'product-mutant'), cycle=mode == 'forced-alias')
    if mode in ('probe', 'product-alias', 'product-missing') and code == 0 and case.get('status') in ('PASS', 'SKIP'):
        status = case['status']
    if timed_out:
        status = 'INCONCLUSIVE'
    if mode in ('present', 'present-ahead', 'product-roundtrip') and status == 'PASS' and case.get('presents') != 24:
        status = 'FAIL'
    # Distinguish real translation-layer coverage from an unexpected Wine built-in route.
    if status == 'PASS' and ('DXVK:' not in logs or 'vkd3d-proton' not in logs):
        status = 'INCONCLUSIVE'
    prefix_dlls = {name: sha(prefix / 'drive_c/windows/system32' / name)
                   for name in ('d3d11.dll', 'dxgi.dll', 'd3d12.dll', 'd3d12core.dll')
                   if (prefix / 'drive_c/windows/system32' / name).is_file()}
    return dict(mode=mode, status=status, code=code, prefix_dlls=prefix_dlls, elapsed_s=round(time.monotonic()-start, 3),
                timed_out=timed_out, observation=case, trace=trace.splitlines(),
                runtime_identity_confirmed='DXVK:' in logs and 'vkd3d-proton' in logs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-only', action='store_true')
    args = parser.parse_args()
    if args.host_only:
        return subprocess.run([sys.executable, str(HERE / 'test_runner.py')]).returncode
    OUT.mkdir(exist_ok=True)
    with result_run(OUT / 'result.json'):
        lock = (OUT / 'fixture.lock').open('w')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('SKIP: this fixture is already running')
            return 0  # Do not overwrite another active invocation's evidence.
        write_result(OUT / 'result.json', {'status': 'RUNNING', 'runtime_coverage': False})
        helper = guide_helpers()
        runtime, missing = prerequisite(helper)
        if missing:
            write_result(OUT / 'result.json', {'status': 'SKIP', 'reason': missing, 'runtime_coverage': False})
            print('SKIP:', missing)
            return 0
        build(helper)
        evidence = identity(runtime)
        results = []
        for mode in ('probe', 'no-present', 'present', 'present-ahead', 'forced-alias', 'mutant-copy',
                     'product-roundtrip', 'product-alias', 'product-missing', 'product-mutant'):
            try:
                helper.no_game()
            except RuntimeError:
                results.append(dict(mode=mode, status='SKIP', observation={}, reason='a Steam game started'))
                break
            result = run_case(runtime, mode)
            results.append(result)
            print(mode + ': ' + result['status'], flush=True)
            if result['status'] == 'SKIP' and mode == 'probe':
                break
        statuses = {r['status'] for r in results}
        status = next((s for s in ('FAIL', 'INCONCLUSIVE', 'SKIP') if s in statuses), 'PASS')
        write_result(OUT / 'result.json', dict(status=status, runtime_coverage=any(
            r['observation'].get('verified', 0) for r in results), runtime=evidence, cases=results))
        print(status + ': see ' + str(OUT / 'result.json'))
        return 0 if status in ('PASS', 'SKIP') else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        write_result(OUT / 'result.json', {'status': 'FAIL', 'reason': str(error), 'runtime_coverage': False})
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
