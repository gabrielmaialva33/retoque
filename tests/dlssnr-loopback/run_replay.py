"""Offline NR-boundary replay for run.py --replay-nr (docs/dlssnr/design/offline-replay-seam.md).

One fresh Proton process per variant replays the same capture through the production DLL's replay adapter. Without
LOOPBACK_REPLAY_CAPTURE the first corpus is recorded here: the moving scene with NR off, every frame captured. Its
DLAA output is exactly what the NR pass receives in that pipeline.

Writes:
- `corpus/capture.json` (docs/dlssnr/design/offline-replay.md v1);
- one `runs/<variant>/run.json` per NR-on variant;
- `runs/off/control.json` for the NR-off control;
- `replay-report.json` with the seam's gates.

Pixel verdicts belong to tools/replay; when that package is present it is run on every candidate against the first
every-frame reference, and its exit codes are recorded, not interpreted here.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

from run_moving import digest, execute

STAGED = ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'nvngx_dlssnr.dll', 'nvngx_dlss.dll', '_nvngx.dll',
          'dlssnr-loopback.exe', 'scene.hlsl')
WIDTH, HEIGHT = 640, 360                    # moving_scene.h
FRAME_MS = 1000.0 / 120.0                   # the moving scene's FrameTimeDeltaInMsec
CREATE_FLAGS = (1 << 0) | (1 << 6)          # IsHDR | AutoExposure, as the moving scene creates DLAA
# The warmup must outlast every start-up gate of every variant: tracked recording (multi-pass, adaptive cadence)
# adds two consecutive 500 ms settle gates, so 160 frames at 120 Hz of logical time (1.33 s) before scoring.
CORPUS_FRAMES = int(os.environ.get('LOOPBACK_REPLAY_FRAMES', '256'))
CORPUS_WARMUP = int(os.environ.get('LOOPBACK_REPLAY_WARMUP', '160'))
# name -> [DlssNr] keys, None for the NR-off control. Order matters: the first every-frame run is the reference.
DEFAULT_VARIANTS = {
    'off': None,
    'every-a': {'Cadence': '1'},
    'every-b': {'Cadence': '1'},
    'cadence2': {'Cadence': '2'},
    'cadence4': {'Cadence': '4'},
    'adaptive4': {'Cadence': '4', 'CadenceAdaptive': 'true'},
}
# A refactor's gate (offline-replay-seam.md): a directory holding runs/<variant>/run.json (and runs/off/control.json)
# of an earlier build on the same capture. Every variant both have must match it decision for decision and bit for bit.
BASELINE = os.environ.get('LOOPBACK_REPLAY_BASELINE')
# Fresh every-frame repeats for the judge's A/A envelope (offline-replay.md: five at least); 0 skips calibration.
CALIBRATE = int(os.environ.get('LOOPBACK_REPLAY_CALIBRATE', '5'))
LINE = re.compile(r'^REPLAY frame=(\d+) present=(\d+) gpu_ms=(-?[\d.]+) recorded=(\d) carried=(\d) interval=(\d+) '
                  r'requested=(\d+) completed=(\d+) result=(-?\d+) skip=(.*)$', re.M)



def replay_mode(env=None):
    env = os.environ if env is None else env
    def option(name, minimum, maximum):
        value = env.get(name, '0')
        if not re.fullmatch(r'[0-9]+', value):
            raise ValueError(f'{name}: expected unsigned decimal')
        number = int(value)
        if number != 0 and not minimum <= number <= maximum:
            raise ValueError(f'{name}: expected 0 or {minimum}..{maximum}')
        return number
    ring = option('LOOPBACK_REPLAY_INFLIGHT', 2, 4)
    cancel = option('LOOPBACK_REPLAY_CANCEL_EVERY', 16, 100000)
    recreate = option('LOOPBACK_REPLAY_RECREATE_EVERY', 16, 100000)
    if not ring and (cancel or recreate):
        raise ValueError('lifecycle stress requires LOOPBACK_REPLAY_INFLIGHT')
    return dict(ring=ring, cancel_every=cancel, recreate_every=recreate,
                completion='gated-oldest' if ring else 'synchronous')


def mode_of(run):
    return run.get('replay_mode', replay_mode({}))


def flight_rows(log, frames, mode):
    """Require every scheduling/readback event, before accepting stress outputs as fenced."""
    if not mode['ring']:
        return {}
    done = re.findall(r'^FLIGHT frame=(\d+) slot=(\d+) executed=(\d) cancelled=(\d) fence=(\d+) completed=(\d+)$', log, re.M)
    reuse = re.findall(r'^FLIGHT_REUSE frame=(\d+) prior=(\d+) age=(\d+) recreated=(\d) pending=(\d+) ready=(\d+)$', log, re.M)
    ids = [f['id'] for f in frames]
    if [int(r[0]) for r in done] != ids or [int(r[0]) for r in reuse] != ids:
        raise ValueError('missing/duplicate/out-of-order flight event')
    result = {}
    n = mode['ring']
    for i, (d, r) in enumerate(zip(done, reuse)):
        frame, slot, executed, cancelled, fence, completed = map(int, d)
        _, prior, age, recreated, pending, ready = map(int, r)
        expected_cancel = bool(mode['cancel_every'] and (i + 1) % mode['cancel_every'] == 0)
        previous_cancel = i >= n and mode['cancel_every'] and (i - n + 1) % mode['cancel_every'] == 0
        expected_recreate = bool(i >= n and mode['recreate_every'] and
                                 (i + 1) % mode['recreate_every'] == 0 and not previous_cancel)
        if (slot != i % n or fence != i + 1 or completed != fence or cancelled != expected_cancel or
                executed != (not expected_cancel) or prior != (ids[i - n] if i >= n else 0) or
                age != (n if i >= n else 0) or recreated != expected_recreate or
                ready != max(0, i - n + 1) or pending != min(i, n - 1)):
            raise ValueError(f'frame {frame}: invalid slot/reset/cancel/recreate/fence schedule')
        result[frame] = dict(slot=slot, executed=bool(executed), cancelled=bool(cancelled),
                             fence=fence, completed_fence=completed, previous_frame=prior,
                             reset_age=age, recreated=bool(recreated), pending_before=pending, completed_before=ready)
    return result


def windows_path(path):
    path = str(Path(path).resolve())
    if any(c.isspace() for c in path):
        raise SystemExit(f'replay plan paths cannot contain whitespace: {path}')
    return 'Z:' + path.replace('/', '\\')


def gpu_identity():
    # Name and driver only: P-state and clocks change between invocations and would fail environment matching.
    out = subprocess.run(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader'],
                         capture_output=True, text=True).stdout.strip().splitlines()
    name, _, driver = (out[0] if out else 'unknown, unknown').partition(',')
    return name.strip() or 'unknown', driver.strip() or 'unknown'


def stage(root, directory):
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    for name in STAGED:
        source = root / name
        if not source.is_file():
            raise SystemExit(f'replay prerequisite missing: {name}')
        (directory / name).symlink_to(source.resolve())


def write_ini(directory, settings):
    enabled = settings is not None
    (directory / 'OptiScaler.ini').write_text(
        '; Offline replay trial. Written by run_replay.py; no game files are modified.\n'
        '[Upscalers]\nDx12Upscaler=dlss\n'
        '[FrameGen]\nFGOutput=none\n'
        '[Menu]\nOverlayMenu=true\n'
        '[Hotfix]\nCheckForUpdate=false\n'
        '[Log]\nLogToFile=true\nLogLevel=2\n'
        '[DlssNr]\nEnabled=' + str(enabled).lower() + '\n'
        'HookMethod=1\nWorkingScale=1\nWhitePointSource=0\nWhitePointScale=1\n'
        'ApplyModel=true\nTransferStrength=1\nColourStrength=1\nSessionGuard=false\n' +
        ''.join(f'{key}={value}\n' for key, value in (settings or {}).items()))


def producer(runner):
    diff = subprocess.run(['git', 'diff', 'HEAD', '--binary'], cwd=runner.ROOT, check=True,
                          capture_output=True).stdout
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=runner.ROOT, check=True, capture_output=True,
                          text=True).stdout.strip()
    return f'{head}+{hashlib.sha256(diff).hexdigest()[:16]}'


def plane(directory, name, dtype, channels, item):
    path = directory / name
    expected = WIDTH * HEIGHT * channels * item
    if path.stat().st_size != expected:
        raise SystemExit(f'{path}: {path.stat().st_size} bytes, expected {expected}')
    return dict(path=name, encoding='raw', dtype=dtype, width=WIDTH, height=HEIGHT, channels=channels,
                row_pitch=WIDTH * channels * item, sha256=digest(path))


def make_corpus(runner, proton, env, root, provenance):
    directory = root / 'corpus'
    stage(root, directory)
    write_ini(directory, None)
    print(f'  replay corpus: moving scene, NR off, {CORPUS_FRAMES} frames all captured', flush=True)
    code = execute([proton, 'run', directory / 'dlssnr-loopback.exe', 'OptiScaler.dll', '--moving-corpus',
                    str(CORPUS_FRAMES)], cwd=directory, env=env, log=directory / 'proton.log', timeout=600)
    log = (directory / 'dlssnr-loopback.log').read_text(errors='replace')
    if code or f'MOVING-SCENE PASS: evaluations={CORPUS_FRAMES} captures={CORPUS_FRAMES}' not in log:
        raise SystemExit(f'replay corpus recording failed ({code}); see {directory}')
    samples = json.loads((directory / 'moving.json').read_text())['samples']
    if [s['frame'] for s in samples] != list(range(CORPUS_FRAMES)):
        raise SystemExit('replay corpus: the moving manifest does not list every frame')
    frames = []
    for s in samples:
        i = s['frame']
        frames.append(dict(
            id=i + 1, present=i + 1, time_ms=round(i * FRAME_MS, 6), reset=i == 0, score=i >= CORPUS_WARMUP,
            jitter=s['jitter'], pre_exposure=1.0, exposure=None, exposure_value=1.0, completion='fence-complete',
            params=dict(create_flags=CREATE_FLAGS, mv_scale=[1.0, 1.0], frame_time_ms=FRAME_MS),
            color=plane(directory, f'f{i}.output', 'f16', 4, 2),
            depth=plane(directory, f'f{i}.depth', 'f32', 1, 4),
            motion=plane(directory, f'f{i}.motion', 'f16', 2, 2)))
    capture = dict(schema='retoque-replay-capture', version=1, complete=True, mode='nr-boundary',
                   source='loopback-moving-scene', clock='recorded-evaluation', color_space='linear-rec709',
                   # DLAA output is de-jittered; its vectors are unjittered scene motion (scene.hlsl).
                   color_jitter='removed',
                   depth=dict(kind='device', inverted=False, threshold=0.02),
                   motion=dict(direction='current-to-previous', units='pixels', scale=[1.0, 1.0],
                               includes_jitter=False),
                   regions={'centre': [WIDTH // 4, HEIGHT // 4, WIDTH // 2, HEIGHT // 2]},
                   provenance=dict(provenance, shader_sha256=digest(runner.HERE / 'scene.hlsl'),
                                   note='DLAA output of the moving scene with NR off; analytic guides'),
                   frames=frames)
    path = directory / 'capture.json'
    path.write_text(json.dumps(capture, indent=1) + '\n')
    return path


def load_capture(path):
    """A capture the seam can replay as recorded, or a refusal naming what it cannot transport (yet)."""
    capture = json.loads(path.read_text())
    if capture.get('schema') != 'retoque-replay-capture' or capture.get('version') != 1 or not capture.get('complete'):
        raise SystemExit(f'{path}: not a complete retoque-replay-capture v1')
    supported = dict(mode='nr-boundary', clock='recorded-evaluation', color_jitter='removed')
    for key, value in supported.items():
        if capture.get(key) != value:
            raise SystemExit(f'{path}: the seam replays {key}={value!r} only (got {capture.get(key)!r})')
    motion = capture.get('motion', {})
    if (motion.get('direction'), motion.get('units'), motion.get('scale'), motion.get('includes_jitter')) != \
            ('current-to-previous', 'pixels', [1.0, 1.0], False):
        raise SystemExit(f'{path}: the seam v1 takes unjittered backward motion in pixels at scale 1')
    base = path.parent
    frames = capture['frames']
    extent = (frames[0]['color']['width'], frames[0]['color']['height'])
    flags = frames[0].get('params', {}).get('create_flags', CREATE_FLAGS)
    previous = None
    for f in frames:
        if f.get('exposure') is not None:
            raise SystemExit(f'frame {f["id"]}: exposure textures are not transported by the seam v1')
        if f.get('params', {}).get('create_flags', CREATE_FLAGS) != flags:
            raise SystemExit(f'frame {f["id"]}: create flags change within the capture')
        if previous and (f['present'] <= previous['present'] or f['time_ms'] <= previous['time_ms']):
            raise SystemExit(f'frame {f["id"]}: presents and times must strictly increase')
        previous = f
        for key, dtype, channels in (('color', 'f16', 4), ('depth', 'f32', 1), ('motion', 'f16', 2)):
            p = f[key]
            # v1 of the seam uploads these formats as they are; anything else is converted by its producer.
            if (p['encoding'], p['dtype'], p['channels']) != ('raw', dtype, channels) or \
                    p['row_pitch'] != p['width'] * channels * (4 if dtype == 'f32' else 2):
                raise SystemExit(f'frame {f["id"]} {key}: the seam v1 takes tight raw {dtype}x{channels}')
            if (p['width'], p['height']) != extent:
                raise SystemExit(f'frame {f["id"]} {key}: the seam v1 takes one constant extent')
            if digest(base / p['path']) != p['sha256']:
                raise SystemExit(f'frame {f["id"]} {key}: digest mismatch')
    return capture


def write_plan(capture, base, path):
    frames = capture['frames']
    width, height = frames[0]['color']['width'], frames[0]['color']['height']
    flags = frames[0].get('params', {}).get('create_flags', CREATE_FLAGS)
    lines = [f'replay-plan 2 {width} {height} {flags} {len(frames)}']
    for i, f in enumerate(frames):
        # The frame time the game passed when it is recorded, else the step of the recorded clock.
        delta = f.get('params', {}).get('frame_time_ms')
        if delta is None:
            delta = (frames[1]['time_ms'] - f['time_ms']) if i == 0 and len(frames) > 1 else \
                f['time_ms'] - frames[i - 1]['time_ms'] if i else 1000.0 / 60.0
        lines.append(' '.join([str(f['id']), str(f['present']), repr(float(f['time_ms'])), repr(float(delta)),
                               str(int(f['reset'])), repr(float(f['jitter'][0])), repr(float(f['jitter'][1])),
                               repr(float(f['pre_exposure']))] +
                              [windows_path(base / f[key]['path']) for key in ('color', 'depth', 'motion')]))
    path.write_text('\n'.join(lines) + '\n')


def requirements(settings, scored):
    """What a variant must show on scored frames for its run to be about what it claims."""
    cadence = int((settings or {}).get('Cadence', '1'))
    adaptive = str((settings or {}).get('CadenceAdaptive', 'false')).lower() == 'true'
    if cadence <= 1:
        return dict(min_model=scored, min_carried=0)
    return dict(min_model=1, min_carried=0 if adaptive else 1)


def kind(row):
    if not row['recorded']:
        return 'skipped'
    if row['carried']:
        return 'carried'
    return 'model' if row['interval'] >= 2 else 'every'


def run_variant(runner, proton, env, root, name, settings, capture, capture_path, environment, producer_id):
    directory = root / 'runs' / name
    stage(root, directory)
    write_ini(directory, settings)
    write_plan(capture, capture_path.parent, directory / 'plan.txt')
    frames = capture['frames']
    print(f'  replay {name}: NR={"off" if settings is None else settings}', flush=True)
    code = execute([proton, 'run', directory / 'dlssnr-loopback.exe', 'OptiScaler.dll', '--replay-nr', 'plan.txt'],
                   cwd=directory, env=dict(env, RETOQUE_REPLAY='1'), log=directory / 'proton.log',
                   timeout=120 + 2 * len(frames))
    log = (directory / 'dlssnr-loopback.log').read_text(errors='replace')
    if code or f'REPLAY PASS: frames={len(frames)}' not in log:
        raise SystemExit(f'replay {name} failed ({code}); see {directory}')
    mode = replay_mode()
    flight = flight_rows(log, frames, mode)
    rows = [dict(id=int(m[0]), present=int(m[1]), gpu_ms=float(m[2]), recorded=int(m[3]), carried=int(m[4]),
                 interval=int(m[5]), requested=int(m[6]), completed=int(m[7]), result=int(m[8]),
                 skip='' if m[9] == '-' else m[9]) for m in LINE.findall(log)]
    if [r['id'] for r in rows] != [f['id'] for f in frames]:
        raise SystemExit(f'replay {name}: one REPLAY line per capture frame expected')
    entries = []
    for f, r in zip(frames, rows):
        out = plane(directory, f'f{f["id"]}.output', 'f16', 4, 2)
        event = flight.get(r['id'], {})
        cancelled = event.get('cancelled', False)
        entries.append(dict(id=r['id'], kind='skipped' if cancelled else kind(r), interval=r['interval'], requested_passes=r['requested'],
                            completed_passes=r['completed'], completion='fence-complete',
                            gpu_ms=r['gpu_ms'] if r['gpu_ms'] >= 0 else None, model_result=r['result'],
                            skip='harness-cancelled-not-submitted' if cancelled else r['skip'] or None,
                            recorded=bool(r['recorded']), production_skip=r['skip'] or None,
                            flight=event or None, output=out))
    scored = sum(1 for f in frames if f['score'])
    record = dict(schema='retoque-replay-run', version=1, complete=True,
                  capture_sha256=digest(capture_path), environment=environment,
                  producer=dict(dll_sha256=digest(root / 'OptiScaler.dll'), commit=producer_id,
                                trial=uuid.uuid4().hex),  # one fresh process per run
                  settings=dict(settings or {}, Enabled=settings is not None),
                  replay_mode=mode, requirements=(dict(min_model=1, min_carried=0) if mode['ring'] else
                                                 requirements(settings, scored)), frames=entries)
    (directory / ('control.json' if settings is None else 'run.json')).write_text(json.dumps(record, indent=1) + '\n')
    return record


def load_baseline(path, capture_sha):
    """The earlier build's manifests, read before any trial of this run can overwrite them."""
    base = Path(path).expanduser().resolve()
    runs = {}
    for directory in sorted((base / 'runs').iterdir()):
        for name in ('run.json', 'control.json'):
            manifest = directory / name
            if manifest.is_file():
                data = json.loads(manifest.read_text())
                if data.get('capture_sha256') != capture_sha:
                    raise SystemExit(f'baseline {directory.name} replayed another capture; pass its capture.json '
                                     'as LOOPBACK_REPLAY_CAPTURE')
                runs[directory.name] = data
    if not runs:
        raise SystemExit(f'{base}: no runs/<variant>/run.json to compare against')
    return runs


def baseline_gates(runs, baseline):
    fields = ('kind', 'interval', 'requested_passes', 'completed_passes', 'model_result', 'skip')
    results = {}
    for name, run in runs.items():
        old = baseline.get(name)
        if old is None:
            results[f'{name}-identical-to-baseline'] = dict(ok=False, detail='variant absent from baseline')
            continue
        if (mode_of(run) != mode_of(old) or run['settings'] != old['settings'] or
                run['environment'] != old['environment'] or run['capture_sha256'] != old['capture_sha256']):
            results[f'{name}-identical-to-baseline'] = dict(ok=False, detail='mode/settings/environment/capture mismatch')
            continue
        same_length = [x['id'] for x in run['frames']] == [x['id'] for x in old['frames']]
        compare_fields = fields + (('recorded', 'production_skip', 'flight') if mode_of(run)['ring'] else ())
        decisions = [x['id'] for x, y in zip(run['frames'], old['frames']) if any(x.get(k) != y.get(k) for k in compare_fields)]
        pixels = [x['id'] for x, y in zip(run['frames'], old['frames'])
                  if x['output']['sha256'] != y['output']['sha256']]
        results[f'{name}-identical-to-baseline'] = dict(
            ok=same_length and not decisions and not pixels,
            detail=f'frames {len(run["frames"])}/{len(old["frames"])}, decisions differ at {decisions[:8]}, '
                   f'pixels differ at {pixels[:8]}; baseline DLL {old["producer"]["dll_sha256"][:12]}, '
                   f'this DLL {run["producer"]["dll_sha256"][:12]}')
    return results



def sync_gates(runs, baseline):
    """History changes after the first differing admission; local frame equality then is not an oracle."""
    results = {}
    fields = ('kind', 'interval', 'requested_passes', 'completed_passes', 'model_result', 'skip')
    for name, run in runs.items():
        old = baseline.get(name)
        if old is None:
            results[f'{name}-sync-comparison'] = dict(ok=False, detail='variant absent from synchronous baseline')
            continue
        if (mode_of(old)['ring'] or run['settings'] != old['settings'] or
                run['capture_sha256'] != old['capture_sha256'] or run['environment'] != old['environment'] or
                [f['id'] for f in run['frames']] != [f['id'] for f in old['frames']]):
            results[f'{name}-sync-comparison'] = dict(ok=False, detail='incompatible synchronous reference')
            continue
        different = [x['id'] for x, y in zip(run['frames'], old['frames'])
                     if any(x[k] != y[k] for k in fields) or (x.get('flight') or {}).get('cancelled')]
        pixels = [x['id'] for x, y in zip(run['frames'], old['frames']) if x['output']['sha256'] != y['output']['sha256']]
        prefix = next((i for i, (x, y) in enumerate(zip(run['frames'], old['frames']))
                       if any(x[k] != y[k] for k in fields) or (x.get('flight') or {}).get('cancelled')),
                      len(run['frames']))
        prefix_bad = [x['id'] for x, y in zip(run['frames'][:prefix], old['frames'][:prefix])
                      if x['output']['sha256'] != y['output']['sha256']]
        results[f'{name}-sync-identical-history-prefix'] = dict(ok=not prefix_bad,
            detail=f'{prefix} frames before admission divergence; mismatches {prefix_bad[:8]}')
        results[f'{name}-sync-comparison'] = dict(ok=None if different else not pixels,
            detail=f'admission differences {len(different)} (first {different[:8]}); pixel differences '
                   f'{len(pixels)} (first {pixels[:8]}); ' +
                   ('changed temporal history: same-mode baseline required' if different else 'same admission history'))
    return results


def stress_gates(capture, runs):
    results = {}
    for name, run in runs.items():
        scored = {f['id'] for f in capture['frames'] if f['score']}
        active = [f for f in run['frames'] if f['id'] in scored and f['recorded'] and f['flight']['executed']]
        cancelled = [f for f in run['frames'] if f['flight']['cancelled']]
        original = {f['id']: f['color']['sha256'] for f in capture['frames']}
        mode = mode_of(run)
        by_id = {f['id']: f for f in run['frames']}
        cancelled_nr = [f for f in cancelled if f['recorded']]
        recreated_nr = [f for f in run['frames'] if f['flight']['recreated'] and
                        by_id[f['flight']['previous_frame']]['recorded'] and
                        by_id[f['flight']['previous_frame']]['flight']['executed']]
        if mode['cancel_every']:
            results[f'{name}-cancelled-NR-exercised'] = dict(ok=bool(cancelled_nr),
                detail=f'{len(cancelled_nr)} cancelled lists contained NR commands')
        if mode['recreate_every']:
            results[f'{name}-released-NR-exercised'] = dict(ok=bool(recreated_nr),
                detail=f'{len(recreated_nr)} released lists previously executed NR')
        pending = [f['flight']['pending_before'] for f in run['frames'] if f['recorded']]
        results[f'{name}-pending-GPU-work-exercised'] = dict(ok=mode['ring'] - 1 in pending,
            detail=f'pending submissions before NR recording: {sorted(set(pending))}')
        results[f'{name}-stress-active'] = dict(ok=bool(active), detail=f'{len(active)} scored NR recordings executed')
        results[f'{name}-cancelled-input-identical'] = dict(
            ok=all(f['output']['sha256'] == original[f['id']] for f in cancelled),
            detail=f'{len(cancelled)} discarded lists, copy-only fenced output')
        results[f'{name}-stress-model-success'] = dict(
            ok=all(f['model_result'] == 1 or f['kind'] == 'carried' for f in active),
            detail='successful model result for executed recorded non-carried frames')
    return results


def gates(capture, runs):
    """The seam's own acceptance (offline-replay-seam.md, Validation). Pixel quality is the judge's."""
    if runs and mode_of(next(iter(runs.values())))['ring']:
        return stress_gates(capture, runs)
    scored = [i for i, f in enumerate(capture['frames']) if f['score']]
    results = {}

    def check(name, ok, detail=''):
        results[name] = dict(ok=bool(ok), detail=detail)

    for name, run in runs.items():
        if name != 'off':
            skipped = [run['frames'][i]['id'] for i in scored if run['frames'][i]['kind'] == 'skipped']
            check(f'{name}-no-scored-frame-skipped', not skipped,
                  f'skipped {skipped[:8]} ({run["frames"][scored[0]]["skip"] if skipped else ""})')
    off = runs.get('off')
    if off:
        same = sum(e['output']['sha256'] == f['color']['sha256'] for e, f in zip(off['frames'], capture['frames']))
        check('off-leaves-every-byte', same == len(capture['frames']), f'{same}/{len(capture["frames"])} identical')
        check('off-records-nothing', all(e['kind'] == 'skipped' for e in off['frames']))
    for name in ('every-a', 'every-b'):
        run = runs.get(name)
        if run:
            bad = [run['frames'][i]['id'] for i in scored
                   if run['frames'][i]['kind'] != 'every' or run['frames'][i]['model_result'] != 1 or
                   run['frames'][i]['completed_passes'] != run['frames'][i]['requested_passes']]
            check(f'{name}-model-every-scored-frame', not bad, f'bad frames {bad[:8]}')
    for name, interval in (('cadence2', 2), ('cadence4', 4)):
        run = runs.get(name)
        if run:
            kinds = [run['frames'][i]['kind'] for i in scored]
            intervals = {run['frames'][i]['interval'] for i in scored}
            check(f'{name}-carries', kinds.count('carried') > 0 and intervals == {interval},
                  f'carried {kinds.count("carried")}/{len(kinds)}, intervals {sorted(intervals)}')
    run = runs.get('adaptive4')
    if run:
        intervals = [run['frames'][i]['interval'] for i in scored]
        check('adaptive-reports-intervals', set(intervals) <= {1, 2, 3, 4} and all(intervals),
              f'interval histogram { {k: intervals.count(k) for k in sorted(set(intervals))} }')
    a, b = runs.get('every-a'), runs.get('every-b')
    if a and b:
        fields = ('kind', 'interval', 'requested_passes', 'completed_passes', 'model_result', 'skip')
        differing = [x['id'] for x, y in zip(a['frames'], b['frames']) if any(x[k] != y[k] for k in fields)]
        check('repeat-decisions-identical', not differing, f'differing frames {differing[:8]}')
        identical = sum(x['output']['sha256'] == y['output']['sha256'] for x, y in zip(a['frames'], b['frames']))
        # Informational: bitwise equality between fresh processes is observed, not promised by NGX. The judge's
        # A/A calibration, not this line, sets what a repeat may differ by.
        results['repeat-pixels-identical-frames'] = dict(ok=None, detail=f'{identical}/{len(a["frames"])} frames '
                                                         'bitwise identical across two fresh processes')
    return results


def judge_python():
    # tools/replay picks its NumPy backend when importable; uv supplies NumPy in a throwaway environment, never in
    # system Python. Without uv the scalar reference runs, about twenty times slower.
    return ['uv', 'run', '--no-project', '-q', '--with', 'numpy', 'python'] if shutil.which('uv') else [sys.executable]


def calibrate(runner, root, capture_path, repeats):
    out = root / 'judge' / 'calibration.json'
    p = subprocess.run(judge_python() + ['-m', 'tools.replay', 'calibrate', '--capture', str(capture_path),
                                         '--reference', str(root / 'runs' / 'every-a' / 'run.json'), '--out',
                                         str(out)] +
                       [arg for name in repeats for arg in ('--repeat', str(root / 'runs' / name / 'run.json'))],
                       cwd=runner.ROOT, capture_output=True, text=True)
    data = json.loads(out.read_text()) if out.is_file() else {}
    ok = p.returncode == 0 and data.get('verdict') != 'INVALID'
    # A calibration file carries no verdict when it succeeds, only its bounds.
    return out if ok else None, dict(exit=p.returncode, verdict='CALIBRATED' if ok else data.get('verdict'),
                                     reasons=data.get('reasons'), bounds=data.get('bounds', {}).get('all'),
                                     stderr=p.stderr[-2000:])


def judge(runner, root, capture_path, runs, calibration=None):
    package = runner.ROOT / 'tools/replay'
    if not (package / '__main__.py').is_file():
        return dict(available=False)
    # A refactor's gate is the bit-exact baseline comparison; judging every candidate adds ~45 s each and decides
    # nothing there. LOOPBACK_REPLAY_JUDGE=0 skips it.
    if os.environ.get('LOOPBACK_REPLAY_JUDGE', '1') == '0':
        return dict(available=True, skipped='LOOPBACK_REPLAY_JUDGE=0')
    verdicts = {}
    reference = root / 'runs' / 'every-a' / 'run.json'
    scored = {f['id'] for f in json.loads(capture_path.read_text())['frames'] if f['score']}
    for name in runs:
        if name in ('off', 'every-a') or name.startswith('aa-'):
            continue
        # Extra passes come up one at a time, 500 ms of logical time apart (the chain's creation spacing). A
        # chain still growing on scored frames is a partial chain to the judge; such a run is compared bit for
        # bit against a baseline, but judging it needs a corpus whose warmup outlasts the growth.
        growing = [e['id'] for e in runs[name]['frames'] if e['id'] in scored and e['kind'] in ('every', 'model')
                   and e['completed_passes'] != e['requested_passes']]
        if growing:
            verdicts[name] = dict(exit=None, verdict='NOT-JUDGED', reasons=[
                f'chain still growing on {len(growing)} scored frames (first {growing[0]}); lengthen the warmup'],
                backend=None, stderr='')
            continue
        out = root / 'judge' / name
        if out.exists():
            shutil.rmtree(out)
        p = subprocess.run(judge_python() + ['-m', 'tools.replay', '--capture', str(capture_path), '--reference',
                                             str(reference), '--candidate', str(root / 'runs' / name / 'run.json'),
                                             '--out', str(out)] +
                           (['--calibration', str(calibration)] if calibration else []),
                           cwd=runner.ROOT, capture_output=True, text=True)
        result = out / 'result.json'
        data = json.loads(result.read_text()) if result.is_file() else {}
        verdicts[name] = dict(exit=p.returncode, verdict=data.get('verdict'), reasons=data.get('reasons'),
                              backend=data.get('backend'), stderr=p.stderr[-2000:])
    return dict(available=True, verdicts=verdicts)


def run(runner, proton, env, dll):
    mode = replay_mode()
    if mode['ring'] and (CALIBRATE != 0 or os.environ.get('LOOPBACK_REPLAY_JUDGE', '1') != '0'):
        raise SystemExit('inflight stress needs CALIBRATE=0 and JUDGE=0 (deliberate scored skips)')
    root = runner.RUN
    report_path = root / 'replay-report.json'
    report_path.unlink(missing_ok=True)
    provenance = dict(producer=producer(runner), dll_sha256=digest(root / 'OptiScaler.dll'),
                      model_sha256=digest(root / 'nvngx_dlssnr.dll'),
                      fixture_sources={n: digest(runner.HERE / n) for n in
                                       ('harness.cpp', 'moving_scene.h', 'replay_nr.h', 'scene.hlsl',
                                        'run_replay.py', 'run.py')})
    gpu, driver = gpu_identity()
    environment = dict(model_sha256=provenance['model_sha256'], gpu=gpu, driver=driver, runtime=proton.parent.name)
    supplied = os.environ.get('LOOPBACK_REPLAY_CAPTURE')
    capture_path = Path(supplied).expanduser().resolve() if supplied else make_corpus(runner, proton, env, root,
                                                                                         provenance)
    capture = load_capture(capture_path)
    baseline = load_baseline(BASELINE, digest(capture_path)) if BASELINE else None
    variants = json.loads(os.environ['LOOPBACK_REPLAY_VARIANTS']) if os.environ.get('LOOPBACK_REPLAY_VARIANTS') \
        else DEFAULT_VARIANTS
    if 'every-a' in variants and CALIBRATE > 0 and (runner.ROOT / 'tools/replay/__main__.py').is_file():
        if CALIBRATE < 5:
            raise SystemExit('LOOPBACK_REPLAY_CALIBRATE needs at least five repeats (offline-replay.md)')
        variants = dict(variants, **{f'aa-{i}': {'Cadence': '1'} for i in range(1, CALIBRATE + 1)})
    runs = {name: run_variant(runner, proton, env, root, name, settings, capture, capture_path, environment,
                              provenance['producer']) for name, settings in variants.items()}
    results = gates(capture, runs)
    if baseline:
        results.update(baseline_gates(runs, baseline))
    sync_baseline = os.environ.get('LOOPBACK_REPLAY_SYNC_BASELINE')
    if sync_baseline:
        results.update(sync_gates(runs, load_baseline(sync_baseline, digest(capture_path))))
    repeats = [name for name in runs if name.startswith('aa-')]
    calibration, calibration_report = calibrate(runner, root, capture_path, repeats) if repeats else (None, None)
    if repeats:
        results['aa-calibration'] = dict(ok=calibration is not None,
                                         detail=f'{len(repeats)} fresh every-frame repeats, verdict '
                                                f'{calibration_report["verdict"]}')
    verdicts = judge(runner, root, capture_path, runs, calibration)
    # The judge's strict reader is the compatibility test of everything written above.
    for name, verdict in verdicts.get('verdicts', {}).items():
        results[f'{name}-judge-accepts-manifests'] = dict(
            ok=None if verdict['verdict'] == 'NOT-JUDGED' else verdict['verdict'] not in (None, 'INVALID'),
            detail=f'verdict {verdict["verdict"]}, exit {verdict["exit"]}' +
                   (f' ({verdict["reasons"][0]})' if verdict['verdict'] == 'NOT-JUDGED' else ''))
    # The control: an every-frame repeat in the candidate's place must pass its own calibrated envelope.
    if calibration and 'every-b' in verdicts.get('verdicts', {}):
        verdict = verdicts['verdicts']['every-b']
        results['every-b-passes-calibrated-judge'] = dict(ok=verdict['verdict'] == 'PASS',
                                                          detail=f'{verdict["verdict"]}: {verdict["reasons"]}')
    if calibration_report:
        verdicts['calibration'] = calibration_report
    report = dict(schema='retoque-replay-seam-report', version=1, capture=str(capture_path),
                  capture_sha256=digest(capture_path), provenance=provenance, environment=environment,
                  gates=results, judge=verdicts)
    report_path.write_text(json.dumps(report, indent=1) + '\n')
    for name, result in results.items():
        mark = 'PASS' if result['ok'] else 'INFO' if result['ok'] is None else 'FAIL'
        print(f'  {mark} {name}: {result["detail"]}', flush=True)
    print(f'  replay report: {report_path}', flush=True)
    if any(r['ok'] is False for r in results.values()):
        raise SystemExit('REPLAY SEAM FAIL: see replay-report.json')
    print('REPLAY SEAM PASS', flush=True)
