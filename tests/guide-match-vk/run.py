#!/usr/bin/env python3
"""Build and run the Vulkan guide resample on a real Vulkan device under Wine, and judge it.

Compiles the production retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.cpp (and its SPIR-V header) with a pch shim beside
harness.cpp in a private copy of the msvc-wine prefix, runs it in an isolated Wine prefix on winevulkan (the host's
driver) under the Khronos validation layer's core and synchronization checks, each proved active by a negative
control, and requires every read-back texel to be exactly DlssNr::GuideMatch::PointSource's prediction and no
validation error. docs/dlssnr/design/reduced-scale-guides-vk.md. Never loads NGX, OptiScaler.dll or a game. SKIP (exit
0) when the device cannot hold the outputs (DlssNr_GuideMatch_Vk::Unsupported says why)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
WORK = OUT / 'run'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
PASS_DIR = ROOT / 'retoque/shaders/dlssnr'
EXE = 'guide-match-vk-harness.exe'

# The shapes the harness must report, each with every texel compared.
EXPECTED = {'doom_quality_half', 'display_motion', 'route_harness', 'half_d32s8', 'unaligned', 'doom_inflight',
            'one_buffer_x20', 'resubmit_after_17'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    files = [HERE / 'harness.cpp', HERE / 'run.py', HERE / 'stubs/pch.h', PASS_DIR / 'DlssNr_GuideMatch.h',
             PASS_DIR / 'DlssNr_GuideMatch_Vk.h', PASS_DIR / 'DlssNr_GuideMatch_Vk.cpp',
             PASS_DIR / 'precompile/DlssNr_Guides_Shader_Vk.h']
    return {str(p.relative_to(ROOT)): digest(p) for p in files}


def refuse_if_prefix_busy():
    # A compiler or shader build running in the shared prefix (CLAUDE.md, build stalls). Processes, not command lines:
    # another agent's `flock ... build-local.sh` waiting for the lock this suite holds is not a build in flight.
    busy = subprocess.run(['pgrep', '-a', r'^(MSBuild|CL|dxc)\.exe$'], capture_output=True, text=True)
    if busy.returncode == 0:
        raise RuntimeError('the msvc-wine prefix is busy (build or dxc running); run again after it finishes:\n'
                           + busy.stdout)


def command(args, *, env, logfile, cwd=ROOT, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f'exit {p.returncode}: {logfile}\n' + logfile.read_text(errors='replace')[-4000:])


def build():
    refuse_if_prefix_busy()
    build_prefix = OUT / 'build-prefix'
    if not build_prefix.exists():
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(build_prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(build_prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    WORK.mkdir(parents=True, exist_ok=True)
    objdir = OUT / 'obj'
    objdir.mkdir(exist_ok=True)
    try:
        command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
                 # The stub comes first so that "pch.h" resolves to the test shim.
                 '/I' + str(HERE / 'stubs'), '/I' + str(ROOT / 'retoque'), '/I' + str(ROOT / 'external/vulkan/include'),
                 HERE / 'harness.cpp', PASS_DIR / 'DlssNr_GuideMatch_Vk.cpp',
                 '/Fo' + str(objdir) + '/', '/Fe' + str(WORK / EXE), '/link',
                 '/LIBPATH:' + str(ROOT / 'OptiScaler/library/vulkan'), 'vulkan-1.lib'],
                env=env, logfile=OUT / 'build.log', timeout=900)
    finally:
        # This prefix belongs only to this suite; stopping its server touches nothing else.
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime missing')
    for dll in redists[-1].glob('*.dll'):
        (WORK / dll.name).unlink(missing_ok=True)
        shutil.copy2(dll, WORK / dll.name)
    (OUT / 'manifest.json').write_text(json.dumps({
        'sources': sources(),
        'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'binaries': {p.name: digest(p) for p in WORK.iterdir() if p.suffix in ('.dll', '.exe')},
    }, indent=2) + '\n')


def runtime_env():
    return dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
                WINEDLLOVERRIDES='vcruntime140=n;vcruntime140_1=n;msvcp140=n',
                VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
                VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0')


def check_manifest():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    if manifest['sources'] != sources():
        changed = sorted(k for k in set(manifest['sources']) | set(sources())
                         if manifest['sources'].get(k) != sources().get(k))
        raise RuntimeError('--run-only with sources changed since the build: ' + ', '.join(changed))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    args = parser.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    if not args.run_only:
        build()
    else:
        check_manifest()
    if args.build_only:
        print('BUILD OK')
        return 0

    env = runtime_env()
    report_path = WORK / 'guide-match-report.json'
    report_path.unlink(missing_ok=True)
    try:
        command(['wine', WORK / EXE, '--report', 'guide-match-report.json'], env=env, cwd=WORK,
                logfile=OUT / 'run.log', timeout=900)
    finally:
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)

    report = json.loads(report_path.read_text())
    result = {'status': 'PASS', 'failures': [], 'device': report.get('device')}
    if report.get('skipped'):
        result.update(status='SKIP', reason=report['skipped'])
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'SKIP: {report["skipped"]}')
        return 0

    failures = result['failures']
    seen = {c['name']: c for c in report.get('cases', [])}
    for name in sorted(EXPECTED - set(seen)):
        failures.append(f'case {name} missing from the report')
    for name, c in seen.items():
        if c['skipped']:
            # Only the depth/stencil view may be refused by a device (D32_SFLOAT_S8_UINT is optional to sample).
            if name != 'half_d32s8':
                failures.append(f'{name}: skipped')
            continue
        # resubmit_after_17 judges its first evaluate twice (before and after the other 17) and the 17 once.
        want = c['out'][0] * c['out'][1] * (c['frames'] + 2 if name == 'resubmit_after_17' else c['frames'])
        if c['texels'] != want:
            failures.append(f'{name}: {c["texels"]} texels compared, expected {want}')
        if c['depth_mismatches'] or c['motion_mismatches']:
            failures.append(f'{name}: {c["depth_mismatches"]} depth and {c["motion_mismatches"]} motion texels differ '
                            'from PointSource (artifacts/run.log has the first)')
    if not report.get('validation_active'):
        failures.append('the validation layer was not active (the negative control never reached the messenger)')
    if not report.get('sync_validation_active'):
        failures.append('the synchronization checks were not active (two unordered clears were not reported)')
    if report.get('validation_errors', 1) != 0:
        failures.append(f'{report.get("validation_errors")} validation error(s); see artifacts/run.log')

    print(f'device: {report.get("device")}; validation and synchronization checks active, '
          f'{report.get("validation_errors")} error(s)')
    for name, c in seen.items():
        print(f'  {name:<20} {c["out"][0]}x{c["out"][1]} x {c["frames"]} frame(s), {c["in_flight"]} in flight: '
              f'{c["texels"]} texels, {c["depth_mismatches"]} depth / {c["motion_mismatches"]} motion mismatches'
              f'{" (skipped)" if c["skipped"] else ""}')

    result['status'] = 'FAIL' if failures else 'PASS'
    result['cases'] = report.get('cases', [])
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if failures:
        for failure in failures:
            print('FAIL:', failure)
        return 1
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
