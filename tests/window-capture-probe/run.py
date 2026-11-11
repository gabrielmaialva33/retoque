#!/usr/bin/env python3
"""NR on any window, phase 0 (docs/dlssnr/design/nr-anything.md): the window-capture probe.

On this Linux machine the suite only BUILDS probe.exe with the MSVC kit (under the shared compiler-prefix lock) and
checks its source contract; it never runs it. The probe is run by hand on native Windows (and, if wanted, inside a
Proton prefix), and `--parse FILE` turns its output into the HRESULT table the design asks for:

    probe.exe [--window 0xHANDLE] > probe-windows.txt          (on Windows)
    python3 tests/window-capture-probe/run.py --parse probe-windows.txt
"""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
sys.path.insert(0, str(ROOT / 'tools'))
from build_local import prefix_lease  # noqa: E402  the same non-reentrant lock tools/build-local.sh takes

CHECKS = ('environment', 'winrt', 'wgc-supported', 'wgc-cursor-api', 'wgc-border-api', 'dxgi-factory', 'adapter',
          'd3d11-device', 'd3d11-shared-fences', 'd3d12-device', 'd3d12-queue', 'own-window', 'own-affinity',
          'own-capture-item', 'own-capture-frame', 'target', 'target-affinity', 'target-capture-item',
          'target-capture-frame', 'overlay-window', 'overlay-exclude-from-capture', 'overlay-composition-swapchain',
          'overlay-dcomp-device', 'overlay-dcomp-target', 'overlay-present', 'overlay-drain', 'dxgi-duplicate-output',
          'dxgi-acquire-frame')


def contract():
    source = (HERE / 'probe.cpp').read_text()
    for name in CHECKS:
        assert f'"{name}"' in source or f'"{name.rsplit("-", 1)[0]}"' in source, f'probe check {name} is named'
    code = re.sub(r'//[^\n]*', '', source)
    for forbidden in ('OptiScaler', 'nvngx', 'LoadLibrary'):
        assert forbidden not in code, f'the probe never touches {forbidden}'
    assert 'NeuRotic beae4eea' in source and 'Magpie 69d6105d' in source, 'attribution kept'


def build():
    OUT.mkdir(exist_ok=True)
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all')
    log = OUT / 'build-probe.log'
    # msbuild adds the SDK's C++/WinRT headers itself; a bare cl needs them named.
    kits = sorted((MSVC.parent.parent / 'Windows Kits/10/Include').glob('*/cppwinrt'))
    if not kits:
        raise SystemExit(f'FAIL: no C++/WinRT headers under {MSVC.parent.parent}')
    winrt = 'Z:' + str(kits[-1]).replace('/', '\\')
    command = [str(MSVC / 'cl'), '/nologo', '/std:c++20', '/EHsc', '/MT', '/O1', '/W3', '/permissive-', '/utf-8',
               '/bigobj', '/I' + winrt, str(HERE / 'probe.cpp'), '/Fo' + str(OUT / 'probe.obj'),
               '/Fe' + str(OUT / 'probe.exe'),
               '/link', 'windowsapp.lib', 'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'dcomp.lib', 'dwmapi.lib',
               'user32.lib', 'gdi32.lib']
    with prefix_lease(), log.open('w') as stream:
        result = subprocess.run(command, cwd=OUT, env=env, stdin=subprocess.DEVNULL, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=900)
    text = log.read_text(errors='replace')
    if result.returncode != 0 or not (OUT / 'probe.exe').is_file():
        raise SystemExit(f'FAIL: probe.exe did not build ({result.returncode}); {log}\n' + text[-3000:])
    warnings = [line for line in text.splitlines() if ': warning C' in line]
    if warnings:
        raise SystemExit('FAIL: probe.cpp builds with warnings:\n' + '\n'.join(warnings[:20]))
    return OUT / 'probe.exe'


def parse(path):
    rows, end = [], None
    for line in Path(path).read_text(errors='replace').splitlines():
        parts = line.split(' ', 4)
        if line.startswith('PROBE-END'):
            end = line
        elif line.startswith('PROBE ') and len(parts) >= 4:
            rows.append({'check': parts[1], 'result': parts[2], 'hr': parts[3].removeprefix('hr='),
                         'detail': parts[4] if len(parts) > 4 else ''})
    if end is None:
        raise SystemExit(f'{path}: no PROBE-END line; the probe did not finish')
    print('| Check | Result | HRESULT | Detail |\n|---|---|---|---|')
    for row in rows:
        print(f"| {row['check']} | {row['result']} | `{row['hr']}` | {row['detail'].replace('|', '/')} |")
    print(end)
    (OUT).mkdir(exist_ok=True)
    (OUT / (Path(path).stem + '.json')).write_text(json.dumps({'rows': rows, 'end': end}, indent=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--parse', metavar='FILE', help="turn a probe run's output into the HRESULT table")
    args = parser.parse_args()
    if args.parse:
        parse(args.parse)
        return
    contract()
    exe = build()
    print(f'PASS: window-capture probe built ({exe.relative_to(ROOT)}, {exe.stat().st_size} bytes); BUILD ONLY: '
          'it was not run. It needs native Windows 10 2004+ (or a Proton prefix) and is run by hand; see README.md')


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:  # the shared prefix lock is busy
        print(f'FAIL: {error}; run again after the other build finishes', file=sys.stderr)
        sys.exit(1)
