#!/usr/bin/env python3
"""Bounded production-DLL D3D12 guide admission probe; run only under an admitted Proton lease."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent


def hash_file(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_evidence(root, expected_sha256, started_ns):
    root = Path(root)
    dll = root / 'OptiScaler.dll'
    try:
        if not dll.is_file() or hash_file(dll) != expected_sha256:
            raise ValueError('missing or changed staged DLL')
        paths = [root / 'dlssnr-loopback.log', root / 'OptiScaler.log', root / 'steam-0.log']
        if any(not p.is_file() or p.stat().st_mtime_ns < started_ns for p in paths):
            raise ValueError('missing or stale runtime evidence')
        harness, opti, trace = [p.read_text(errors='replace') for p in paths]
    except OSError as exc:
        raise ValueError(f'missing runtime evidence: {exc}') from exc
    if 'MOVING-PRESENT delay_ms=30' not in harness:
        raise ValueError('harness did not confirm the 30 ms present delay')
    if 'MOVING-SCENE PASS: evaluations=80 captures=16' not in harness:
        raise ValueError('moving scene did not complete 80 evaluations and 16 pre-present captures')
    windows_dll = ('Z:' + str(dll.resolve()).replace('/', '\\')).casefold()
    loaded = [line for line in trace.splitlines() if 'trace:loaddll:' in line.lower()
              and 'loaded l"' in line.lower() and ': native' in line.lower()
              and ('loaded l\"' + windows_dll + '\"') in line.replace('\\\\', '\\').casefold()]
    if not loaded:
        raise ValueError('loader trace does not identify the staged native DLL')
    admission = ('guide snapshot admitted via same-queue execution order',
                 'guide snapshot admitted via CPU completion proof')
    if not any(marker in opti for marker in admission):
        raise ValueError('no actual present guide snapshot admission')
    pattern = re.compile(r"DLSS-NR present guides, identity applied, D3D12 present: .*?\| (\d+) used the game's depth and motion,", re.M)
    used = sum(int(match) for match in pattern.findall(opti))
    if used <= 0:
        raise ValueError('no D3D12 present consumed game guides')
    return {'guides_used': used, 'loader_line': loaded[0].strip()}


def required_file(path, label):
    value = Path(path).expanduser().resolve()
    if not value.is_file():
        raise ValueError(f'{label} missing: {value}')
    return value



def harness_command(proton, root):
    return [str(proton), 'run', str(root / 'dlssnr-loopback.exe'), 'OptiScaler.dll',
            '--moving-scene', '--moving-present', '--moving-present-delay-ms', '30']

def run_probe(args):
    # Validate all prerequisites before creating a fresh directory; never delete or reuse evidence.
    files = {
        'OptiScaler.dll': required_file(args.dll, 'DLL'),
        'nvngx.dll_dlssnr.dll': required_file(args.forwarder, 'forwarder'),
        'dlssnr-loopback.exe': required_file(args.harness, 'harness'),
        'nvngx_dlssnr.dll': required_file(args.model, 'model'),
        'nvngx_dlss.dll': required_file(args.sr, 'DLSS SR'),
        '_nvngx.dll': required_file(args.core, 'NGX core'),
        'scene.hlsl': required_file(HERE / 'scene.hlsl', 'scene'),
    }
    proton = required_file(args.proton, 'Proton')
    steam = Path(args.steam).expanduser().resolve()
    if not steam.is_dir():
        raise ValueError('Steam client directory missing')
    identities = {name: hash_file(path) for name, path in files.items()}
    identities['proton'] = hash_file(proton)
    root = Path(args.out).expanduser().absolute()
    root.mkdir(exist_ok=False)
    # Only copy the executable and selected DLL; large local NVIDIA inputs remain linked, never copied or redistributed.
    for name, source in files.items():
        dest = root / name
        if name in ('OptiScaler.dll', 'dlssnr-loopback.exe', 'scene.hlsl'):
            shutil.copyfile(source, dest)
        else:
            dest.symlink_to(source)
    (root / 'OptiScaler.ini').write_text(
        '[Upscalers]\nDx12Upscaler=dlss\n[FrameGen]\nFGOutput=none\n'
        '[Menu]\nOverlayMenu=true\n[Hotfix]\nCheckForUpdate=false\n'
        '[Log]\nLogToFile=true\nLogLevel=2\n[DlssNr]\nEnabled=true\n'
        'HookMethod=2\nRequireDlss=true\nWorkingScale=1\nWhitePointSource=0\n'
        'WhitePointScale=1\nApplyModel=true\nTransferStrength=1\nColourStrength=1\nSessionGuard=false\n')
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(root / 'compat'),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), WINEDEBUG='-all,+loaddll',
               SteamGameId='0', PROTON_LOG='1', PROTON_LOG_DIR=str(root))
    (root / 'compat').mkdir()
    run_id = uuid.uuid4().hex
    started_ns = time.time_ns()
    record = {'schema': 'retoque-present-guides-probe', 'version': 1, 'run_id': run_id,
              'started_ns': started_ns, 'status': 'RUNNING', 'inputs_sha256': identities,
              'route': 'D3D12 present, moving scene; no post-present readback',
              'harness_args': harness_command(proton, root)[3:]}
    (root / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    # Same lease and bounded subprocess helper used by the moving-scene harness.
    import run_moving
    try:
        with run_moving.lease(root):
            code = run_moving.execute(harness_command(proton, root),
                                      cwd=root, env=env, log=root / 'proton.log', timeout=300)
        if code != 0:
            raise ValueError(f'harness returned {code}')
        record.update(validate_evidence(root, identities['OptiScaler.dll'], started_ns))
        # Detect input changes during execution, including linked model and forwarder.
        if any(hash_file(root / name) != digest for name, digest in identities.items() if name != 'proton'):
            raise ValueError('an input changed during execution')
        if hash_file(proton) != identities['proton']:
            raise ValueError('Proton changed during execution')
        record['status'] = 'PASS'
    except (Exception, SystemExit) as exc:
        record['status'] = 'FAIL'
        record['error'] = str(exc)
        raise
    finally:
        record['finished_ns'] = time.time_ns()
        (root / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f"PASS: {record['guides_used']} D3D12 presents used captured guides; {root / 'result.json'}")


def compare(before, after):
    records = []
    for directory in (before, after):
        root = Path(directory).resolve()
        record = json.loads((root / 'result.json').read_text())
        if (record.get('schema'), record.get('version'), record.get('status')) != ('retoque-present-guides-probe', 1, 'PASS'):
            raise ValueError('missing successful current result')
        if record.get('harness_args') != ['OptiScaler.dll', '--moving-scene', '--moving-present',
                                         '--moving-present-delay-ms', '30']:
            raise ValueError('result lacks the required harness arguments')
        result = validate_evidence(root, record['inputs_sha256']['OptiScaler.dll'], record['started_ns'])
        for name, digest in record['inputs_sha256'].items():
            if name != 'proton' and (not (root / name).is_file() or hash_file(root / name) != digest):
                raise ValueError(f'input changed since run: {name}')
        if result['guides_used'] != record['guides_used']:
            raise ValueError('result does not match runtime evidence')
        records.append(record)
    a, b = records
    if a['run_id'] == b['run_id']:
        raise ValueError('before and after must be independent runs')
    for name in ('dlssnr-loopback.exe', 'nvngx_dlssnr.dll', 'nvngx_dlss.dll', '_nvngx.dll', 'scene.hlsl', 'proton'):
        if a['inputs_sha256'][name] != b['inputs_sha256'][name]:
            raise ValueError(f'input differs: {name}')
    print('Both DLLs admitted game guides in independent D3D12-present runs.')
    print(f"before: sha256={a['inputs_sha256']['OptiScaler.dll']} used={a['guides_used']}")
    print(f"after:  sha256={b['inputs_sha256']['OptiScaler.dll']} used={b['guides_used']}")
    print('This is not a pixel comparison or D3D11 coverage.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run')
    for flag in ('dll', 'forwarder', 'harness', 'model', 'sr', 'core', 'proton', 'steam', 'out'):
        run.add_argument('--' + flag, required=True)
    check = sub.add_parser('compare')
    check.add_argument('--before', required=True)
    check.add_argument('--after', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'run':
            run_probe(args)
        else:
            compare(args.before, args.after)
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
