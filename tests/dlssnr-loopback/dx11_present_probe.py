#!/usr/bin/env python3
"""Fail-closed, DLL-backed native D3D11 guide/bridge probe. No build is performed."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
import uuid
from present_guides_probe import hash_file, required_file


def validate_evidence(root, expected_sha256, started_ns):
    root = Path(root)
    dll = root / 'OptiScaler.dll'
    try:
        if not dll.is_file() or hash_file(dll) != expected_sha256:
            raise ValueError('missing or changed staged DLL')
        paths = [root / 'dx11-present.log', root / 'OptiScaler.log', root / 'steam-0.log']
        if any(not p.is_file() or p.stat().st_mtime_ns < started_ns for p in paths):
            raise ValueError('missing or stale runtime evidence')
        harness, opti, trace = [p.read_text(errors='replace') for p in paths]
    except OSError as exc:
        raise ValueError(f'missing runtime evidence: {exc}') from exc
    if 'D3D11-PRESENT FAIL:' in harness:
        raise ValueError('harness reported failure')
    success = re.findall(r'^D3D11-PRESENT PASS: evaluations=(\d+) presents=(\d+) elapsed_ms=(\d+)$', harness, re.M)
    if len(success) != 1:
        raise ValueError('missing or duplicate harness completion')
    loop = re.findall(r'^D3D11-PRESENT LOOP-COMPLETE: evaluations=(\d+) presents=(\d+) elapsed_ms=(\d+)$', harness, re.M)
    if len(loop) != 1 or loop[0] != success[0]:
        raise ValueError('missing or inconsistent completed-loop marker')
    evaluations, presents, elapsed = map(int, success[0])
    if evaluations != 200 or presents != 200 or elapsed < 6000:
        raise ValueError('incomplete evaluate/present window')
    windows_dll = ('Z:' + str(dll.resolve()).replace('/', '\\')).casefold()
    loaded = [line for line in trace.splitlines() if 'trace:loaddll:' in line.lower()
              and ': native' in line.lower()
              and ('loaded l"' + windows_dll + '"') in line.replace('\\\\', '\\').casefold()]
    if not loaded:
        raise ValueError('loader trace does not identify the staged native DLL')
    if ('Creating new DLSS feature' not in opti or 'CreateFeature failed' in opti
            or 'changing backend to ' in opti):
        raise ValueError('native DLSS selection or creation failed; fallback is not accepted')
    carried = sum(int(n) for n in re.findall(
        r'DLSS-NR native D3D11 guides: \d+ evaluates in [\d.]+ s \| (\d+) carried to the present pass', opti))
    used = sum(int(n) for n in re.findall(
        r"DLSS-NR present guides, identity applied, D3D11 bridge: .*?\| (\d+) used the game's depth and motion,", opti))
    if carried <= 0 or used <= 0:
        raise ValueError('missing native D3D11 carried or bridge-used coverage')
    return {'carried': carried, 'guides_used': used, 'evaluations': evaluations,
            'presents': presents, 'elapsed_ms': elapsed, 'loader_line': loaded[0].strip()}


def run_probe(args):
    files = {'OptiScaler.dll': required_file(args.dll, 'DLL'),
             'nvngx.dll_dlssnr.dll': required_file(args.forwarder, 'forwarder'),
             'dx11-present.exe': required_file(args.harness, 'harness'),
             'nvngx_dlssnr.dll': required_file(args.model, 'model'),
             'nvngx_dlss.dll': required_file(args.sr, 'DLSS SR'),
             '_nvngx.dll': required_file(args.core, 'NGX core')}
    proton = required_file(args.proton, 'Proton')
    steam = Path(args.steam).expanduser().resolve()
    if not steam.is_dir():
        raise ValueError('Steam client directory missing')
    identities = {name: hash_file(path) for name, path in files.items()}
    identities['proton'] = hash_file(proton)
    root = Path(args.out).expanduser().absolute()
    root.mkdir(exist_ok=False)
    for name, source in files.items():
        dest = root / name
        if name in ('OptiScaler.dll', 'dx11-present.exe'):
            shutil.copyfile(source, dest)
        else:
            dest.symlink_to(source)
    (root / 'OptiScaler.ini').write_text(
        '[Upscalers]\nDx11Upscaler=dlss\n[DLSS]\nEnabled=true\n[FrameGen]\nFGOutput=none\n'
        '[Menu]\nOverlayMenu=true\n[Hotfix]\nCheckForUpdate=false\n[Log]\nLogToFile=true\nLogLevel=2\n'
        '[DlssNr]\nEnabled=true\nHookMethod=2\nWorkingScale=1\nWhitePointSource=0\n'
        'WhitePointScale=1\nApplyModel=true\nSessionGuard=false\n')
    (root / 'compat').mkdir()
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(root / 'compat'),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), WINEDEBUG='-all,+loaddll,+seh',
               SteamGameId='0', PROTON_LOG='1', PROTON_LOG_DIR=str(root))
    started_ns = time.time_ns()
    record = {'schema': 'retoque-dx11-present-probe', 'version': 1, 'run_id': uuid.uuid4().hex,
              'started_ns': started_ns, 'status': 'RUNNING', 'inputs_sha256': identities,
              'harness_args': ['OptiScaler.dll'], 'route': 'native D3D11 DLSS to D3D11 bridge'}
    (root / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    import run_moving
    try:
        with run_moving.lease(root):
            code = run_moving.execute([proton, 'run', root / 'dx11-present.exe', 'OptiScaler.dll'],
                                      cwd=root, env=env, log=root / 'proton.log', timeout=300)
        if code:
            raise ValueError(f'harness returned {code}; native DLSS preflight or runtime failed')
        record.update(validate_evidence(root, identities['OptiScaler.dll'], started_ns))
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
    print(f"PASS: carried={record['carried']}, D3D11 bridge used={record['guides_used']}; {root / 'result.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('dll', 'forwarder', 'harness', 'model', 'sr', 'core', 'proton', 'steam', 'out'):
        parser.add_argument('--' + flag, required=True)
    try:
        run_probe(parser.parse_args())
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
