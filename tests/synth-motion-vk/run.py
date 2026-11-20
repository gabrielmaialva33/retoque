#!/usr/bin/env python3
"""Build and run the Vulkan synthesized-motion estimator on a real Vulkan device under Wine, and score it.

Compiles the production retoque/shaders/synth_motion/SynthMotion_Vk.cpp with a pch shim beside harness.cpp, runs it in
an isolated Wine prefix on winevulkan (the host's driver) under the Khronos validation layer's core and synchronization
checks, and judges synth-motion-report.json with tests/synth-motion-d3d12's thresholds (its judge()): the two
estimators share the frames, the sequences and the scores (sequences.h), so they are held to the same numbers. Never
loads NGX, OptiScaler.dll or a game.

On top of the D3D12 suite's judgement: the validation layer must have been active (a deliberately invalid fence flag
reached the messenger with its VUID), its synchronization checks too (two unordered clears were reported as a hazard),
and it must have reported no error; a pan and the cut also run three frames in flight. The harness reports the GPU's
own scene-change value for every frame, which that judgement holds to FidelityFX's formula evaluated on the host.

A second run (ROUNDS) puts the in-flight sequences and a timing one through the estimator 25 times with nothing of the
host's between frames, so that frames really are on the GPU together: every round must leave the scene-change output
the first one left, bit for bit, and the first one's must be the formula's.

SKIP (exit 0) when the device cannot run the estimator (SynthMotion::VkUnsupported says why)."""
import argparse
import hashlib
import importlib.util
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
ESTIMATOR = ROOT / 'retoque/shaders/synth_motion'
D3D12_SUITE = ROOT / 'tests/synth-motion-d3d12'
EXE = 'synth-motion-vk-harness.exe'
ROUND_SEQUENCES = ('time_1080p', 'pan_+4x_inflight', 'cut_inflight')  # in the harness's order
ROUND_COUNT = 25
ROUNDS = ['--fast', '--only', ','.join(ROUND_SEQUENCES), '--repeat', str(ROUND_COUNT)]


def d3d12_runner():
    """The D3D12 suite's run.py, for its thresholds and judge(): one contract, one judgement."""
    spec = importlib.util.spec_from_file_location('synth_motion_d3d12_run', D3D12_SUITE / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    precompile = ESTIMATOR / 'precompile'
    files = ([HERE / 'harness.cpp', HERE / 'run.py', D3D12_SUITE / 'sequences.h', D3D12_SUITE / 'run.py'] +
             sorted((HERE / 'stubs').glob('*.h')) + [ESTIMATOR / 'SynthMotion_Vk.h', ESTIMATOR / 'SynthMotion_Vk.cpp'] +
             sorted(precompile.glob('*_Shader_Vk.h')))
    return {str(p.relative_to(ROOT)): digest(p) for p in files}


def command(args, *, env, logfile, cwd=ROOT, timeout=600, expected=0):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    if p.returncode != expected:
        raise RuntimeError(f'exit {p.returncode}, expected {expected}: {logfile}\n'
                           + logfile.read_text(errors='replace')[-4000:])


def build(runner):
    if not list((ESTIMATOR / 'precompile').glob('*_Shader_Vk.h')):
        raise RuntimeError('the Vulkan estimator\'s SPIR-V is not generated: run retoque/shaders/synth_motion/'
                           'precompile/build.sh')
    runner.refuse_if_prefix_busy()
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
                 '/I' + str(HERE / 'stubs'), '/I' + str(ESTIMATOR), '/I' + str(ROOT / 'external/vulkan/include'),
                 HERE / 'harness.cpp', ESTIMATOR / 'SynthMotion_Vk.cpp',
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


def judge_rounds(report, runner):
    """The second run: the rounds agree with the first, the first with FidelityFX's formula, frames were in flight."""
    failures, notes = runner.judge_scene_change(report)
    failures = [f'rounds: {f}' for f in failures]
    names = [s['name'] for s in report['sequences']]
    frames = sum(len(s['frames']) for s in report['sequences'])
    if names != list(ROUND_SEQUENCES):
        failures.append(f'rounds: ran {names}, not {list(ROUND_SEQUENCES)}')
    if report.get('rounds') != ROUND_COUNT or report.get('round_frames') != frames * (ROUND_COUNT - 1):
        failures.append(f'rounds: {report.get("round_frames")} frames compared over {report.get("rounds")} rounds, not '
                        f'{frames * (ROUND_COUNT - 1)} over {ROUND_COUNT}')
    if report.get('round_mismatches', 1) != 0:
        failures.append(f'rounds: {report.get("round_mismatches")} of {report.get("round_frames")} frames left another '
                        'scene-change value than the first round did; see artifacts/rounds.log')
    if not report.get('frames_in_flight'):
        failures.append('rounds: no frame was still on the GPU when its objects came round again, so none were in '
                        'flight together')
    if report.get('validation_errors', 1) != 0:
        failures.append(f'rounds: {report.get("validation_errors")} validation error(s); see artifacts/rounds.log')
    notes = [f'rounds: {n}' for n in notes]
    notes.append(f'rounds: {report.get("round_frames")} frames over {ROUND_COUNT - 1} further rounds, '
                 f'{report.get("round_mismatches")} differing from the first; {report.get("frames_in_flight")} of '
                 f'{frames * ROUND_COUNT} still on the GPU when their objects came round')
    return failures, notes


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

    runner = d3d12_runner()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.run_only:
        build(runner)
    else:
        check_manifest()
    if args.build_only:
        print('BUILD OK')
        return 0

    env = runtime_env()
    report_path = WORK / 'synth-motion-report.json'
    rounds_path = WORK / 'synth-motion-rounds.json'
    report_path.unlink(missing_ok=True)
    rounds_path.unlink(missing_ok=True)
    try:
        command(['wine', WORK / EXE, '--report', report_path.name], env=env, cwd=WORK, logfile=OUT / 'run.log',
                timeout=900)
        if not json.loads(report_path.read_text()).get('skipped'):
            command(['wine', WORK / EXE, *ROUNDS, '--report', rounds_path.name], env=env, cwd=WORK,
                    logfile=OUT / 'rounds.log', timeout=900)
    finally:
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)

    report = json.loads(report_path.read_text())
    result = {'status': 'PASS', 'failures': [], 'notes': []}
    if report.get('skipped'):
        result.update(status='SKIP', reason=report['skipped'])
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'SKIP: {report["skipped"]}')
        return 0

    # The in-flight cut is the D3D12 suite's cut sequence run three frames deep: judged as `cut` on its own, so its
    # SceneCut() is not taken for a stray one by the rest.
    main = dict(report, sequences=[s for s in report['sequences'] if s['name'] != 'cut_inflight'])
    failures, rows, timing, extents, notes, overlays = runner.judge(main)
    inflight = [dict(s, name='cut') for s in report['sequences'] if s['name'] == 'cut_inflight']
    if not inflight:
        failures.append('no cut_inflight sequence in the report')
    else:
        cut_failures, cut_rows, _, cut_extents, cut_notes, _ = runner.judge(dict(report, sequences=inflight))
        failures += [f'cut_inflight: {f}' for f in cut_failures]
        rows += [('cut_inflight',) + tuple(row[1:]) for row in cut_rows]
        extents = sorted(set(extents) | set(cut_extents))
        notes += [f'cut_inflight: {n}' for n in cut_notes]
    if not any(s['name'] == 'pan_+4x_inflight' for s in report['sequences']):
        failures.append('no pan_+4x_inflight sequence in the report')
    for missing in {'partial', 'time_3440'} - {s['name'] for s in report['sequences']}:
        failures.append(f'no {missing} sequence in the report')
    round_failures, round_notes = judge_rounds(json.loads(rounds_path.read_text()), runner)
    failures += round_failures
    notes += round_notes
    if not report.get('validation_active'):
        failures.append('the validation layer was not active (the negative control never reached the messenger)')
    if not report.get('sync_validation_active'):
        failures.append('the synchronization checks were not active (two unordered clears were not reported)')
    if report.get('validation_errors', 1) != 0:
        failures.append(f'{report.get("validation_errors")} validation error(s); see artifacts/run.log')
    for width, height, colour_width, colour_height in extents:
        if (width, height) != (colour_width, colour_height):
            failures.append(f'a field of {width}x{height} for a {colour_width}x{colour_height} colour')

    print(f'device: {report.get("device")}; validation active, {report.get("validation_errors")} error(s)')
    print(f'{"sequence":<14} {"EPE":<22} {"within 1 px":<16} {"sign / overlay":<40} {"scene cut":<26} GPU ms')
    for row in rows:
        print(f'{row[0]:<14} {row[1]:<22} {row[2]:<16} {row[3]:<40} {row[4]:<26} {row[5]}')
    for name, t in timing.items():
        print(f'{name}: {t["width"]}x{t["height"]} median {t["median_ms"]:.3f} ms, p90 {t["p90_ms"]:.3f} ms '
              '(clocks UNLOCKED)')
    for note in notes:
        print('NOTE:', note)

    result.update(failures=failures, notes=notes, timing=timing, device=report.get('device'),
                  status='FAIL' if failures else 'PASS')
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if failures:
        for failure in failures:
            print('FAIL:', failure)
        return 1
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
