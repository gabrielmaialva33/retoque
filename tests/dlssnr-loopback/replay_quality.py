"""Versioned, opt-in temporal fixture and its evidence contract (stdlib, no GPU imports).

The profile varies the existing scene's pose index, not its motion vectors. The
test harness renders each actual current/previous pose with the unchanged shader.
"""
import hashlib
import json
import math
import os
from pathlib import Path

PROFILE = 'temporal-v1'
PROFILES = (PROFILE, 'temporal-reverse-v1')
FRAME_MS = 1000.0 / 60.0
FRAMES, WARMUP = 480, 80
PHASES = (
    ('warmup', 0, 80), ('steady', 80, 160), ('motion', 160, 240),
    ('recovery', 240, 320), ('fast-motion', 320, 360), ('cut', 360, 361),
    ('post-cut-recovery', 361, 480),
)
POLICY = Path(__file__).with_name('replay-quality-policy-v1.json')


def select_profile(profile=None, env=None):
    """Reject relabeling a different capture or schedule as the fixed profile."""
    if profile is None:
        return None
    if profile not in PROFILES:
        raise ValueError(f'unknown replay profile: {profile!r}')
    env = os.environ if env is None else env
    if env.get('LOOPBACK_REPLAY_CAPTURE'):
        raise ValueError(f'{profile} records its own corpus; LOOPBACK_REPLAY_CAPTURE is incompatible')
    if env.get('LOOPBACK_REPLAY_RECORD_CHECK') == '1':
        raise ValueError(f'{profile} is incompatible with LOOPBACK_REPLAY_RECORD_CHECK=1: '
                         'the recorder check renders the legacy scene; use --replay-nr for that check')
    for name, expected in (('LOOPBACK_REPLAY_FRAMES', FRAMES), ('LOOPBACK_REPLAY_WARMUP', WARMUP)):
        if name in env and env[name] != str(expected):
            raise ValueError(f'{profile} requires {name}={expected}; remove the contradictory override')
    return profile


def temporal_frames(profile=PROFILE):
    if profile not in PROFILES:
        raise ValueError(f'unknown replay profile: {profile!r}')
    rows, pose = [], 704 if profile == 'temporal-reverse-v1' else 64
    direction = -1 if profile == 'temporal-reverse-v1' else 1
    for phase, first, stop in PHASES:
        for i in range(first, stop):
            previous = pose
            if phase == 'motion':
                pose += direction * 2
            elif phase == 'fast-motion':
                pose += direction * 12
            elif phase == 'cut':
                pose = 32
            reset = i in (0, 360)
            rows.append(dict(frame=i, pose_frame=pose, previous_pose_frame=pose if reset else previous,
                             reset=reset, time_ms=round(i * FRAME_MS, 6), frame_time_ms=FRAME_MS,
                             score=i >= WARMUP, phase=phase))
    return rows


def _moving_plan_text(rows):
    lines = [f'moving-plan 1 {len(rows)}']
    lines += [' '.join((str(f['frame']), str(f['pose_frame']), str(f['previous_pose_frame']),
                        str(int(f['reset'])), repr(f['time_ms']), repr(f['frame_time_ms']))) for f in rows]
    return '\n'.join(lines) + '\n'


def write_moving_plan(path, rows):
    Path(path).write_text(_moving_plan_text(rows))


def _halton(index, base):
    result, factor = 0.0, 1.0
    while index:
        factor /= base
        result += factor * (index % base)
        index //= base
    return result


def validate_samples(manifest, rows):
    """Check the rendered states, material, clocks and projection-history contract.

The inverse of the previous actual MVP times the reported previous MVP must be
identity. At a reset, the current inverse is used instead. This checks transport
of actual matrices, including holds and cuts; it is not a readback pixel oracle.
"""
    if (manifest.get('width'), manifest.get('height')) != (640, 360):
        raise ValueError('temporal corpus extent mismatch')
    if manifest.get('moving_plan_version') != 1:
        raise ValueError('temporal corpus missing moving plan version')
    samples = manifest.get('samples', [])
    if (manifest.get('frames'), manifest.get('measured'), len(samples)) != (len(rows),) * 3:
        raise ValueError('temporal corpus frames missing or duplicated')
    previous = None
    for expected, actual in zip(rows, samples):
        frame = expected['frame']
        for key in ('frame', 'pose_frame', 'previous_pose_frame', 'reset'):
            if type(actual.get(key)) is not type(expected[key]) or actual[key] != expected[key]:
                raise ValueError(f'frame {frame}: {key} differs from moving plan')
        for key, value in (('time_ms', expected['time_ms']), ('frame_time_ms', expected['frame_time_ms']),
                           ('material_frame', float(expected['pose_frame']))):
            got = actual.get(key)
            if type(got) not in (int, float) or not math.isfinite(got) or not math.isclose(
                    got, value, rel_tol=1e-7, abs_tol=1e-5):
                raise ValueError(f'frame {frame}: {key} differs from moving plan')
        for key, size in (('inverse', 16), ('previous', 16), ('jitter', 2)):
            values = actual.get(key)
            if (not isinstance(values, list) or len(values) != size or
                    not all(type(v) in (int, float) and math.isfinite(v) for v in values)):
                raise ValueError(f'frame {frame}: {key} must contain {size} finite values')
        expected_jitter = [_halton(frame % 32 + 1, base) - 0.5 for base in (2, 3)]
        if any(abs(a - b) > 1e-7 for a, b in zip(actual['jitter'], expected_jitter)):
            raise ValueError(f'frame {frame}: jitter must follow the rendered ordinal')
        inverse = actual['inverse'] if expected['reset'] else previous['inverse']
        matrix = actual['previous']
        residual = max(abs(sum(inverse[y*4+k] * matrix[k*4+x] for k in range(4)) - (x == y))
                       for y in range(4) for x in range(4))
        # Matrices are computed in float32 by DirectXMath, then serialized to nine significant digits.
        if not math.isfinite(residual) or residual > 1e-3:
            raise ValueError(f'frame {frame}: previous matrix does not match actual history ({residual:.6g})')
        if previous is not None:
            same_pose = expected['pose_frame'] == rows[frame-1]['pose_frame']
            same_matrix = actual['inverse'] == previous['inverse']
            if same_pose and not same_matrix:
                raise ValueError(f'frame {frame}: held pose changed its inverse matrix')
            if not same_pose and same_matrix:
                raise ValueError(f'frame {frame}: moving pose kept a stationary inverse matrix')
        previous = actual


def write_policy(directory):
    """Snapshot the checked-in bytes for both calibrator and judge, including short replay."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'policy.json'
    path.write_bytes(POLICY.read_bytes())
    return path


def policy_metadata(path):
    data = Path(path).read_bytes()
    return dict(name=POLICY.name, sha256=hashlib.sha256(data).hexdigest(), values=json.loads(data),
                purpose='Numerical comparison against an A/A envelope; perceptual acceptance is not assessed')


def _frame_delta(frames, index):
    frame = frames[index]
    delta = frame.get('params', {}).get('frame_time_ms') or frame.get('frame_time_ms')
    if delta is not None:
        return float(delta)
    if index:
        return frame['time_ms'] - frames[index-1]['time_ms']
    return frames[1]['time_ms'] - frame['time_ms'] if len(frames) > 1 else 1000.0 / 60.0


def _validate_capture_identity(capture, profile):
    if profile not in PROFILES:
        raise ValueError(f'unknown replay profile: {profile!r}')
    if not isinstance(capture, dict):
        raise ValueError('temporal capture must be an object')
    provenance = capture.get('provenance', {})
    if not isinstance(provenance, dict):
        raise ValueError('temporal capture provenance must be an object')
    nested = provenance.get('corpus', {})
    if not isinstance(nested, dict):
        raise ValueError('temporal corpus metadata must be an object')
    for identity in (capture, provenance, nested):
        if 'profile' in identity and identity['profile'] != profile:
            raise ValueError('temporal capture declared profile mismatch')
        if 'source' in identity and identity['source'] != 'loopback-moving-scene':
            raise ValueError('temporal capture declared source mismatch')
        if 'moving_plan_sha256' in identity:
            expected = hashlib.sha256(_moving_plan_text(temporal_frames(profile)).encode()).hexdigest()
            if identity['moving_plan_sha256'] != expected:
                raise ValueError('temporal capture moving plan digest differs from declared profile')


def validate_temporal_capture(capture, profile=PROFILE):
    """Validate the selected fixed capture contract, defaulting to temporal-v1.

    A nested corpus summary is optional: the producer calls this while constructing
    that summary. Any identity or moving-plan binding already declared must agree.
    """
    _validate_capture_identity(capture, profile)
    frames = capture.get('frames')
    if not isinstance(frames, list) or len(frames) != FRAMES:
        raise ValueError('temporal corpus frame count mismatch')
    for i, frame in enumerate(frames):
        if not isinstance(frame, dict) or type(frame.get('id')) is not int or frame['id'] != i + 1:
            raise ValueError(f'frame {i}: temporal frame id must be {i + 1}')
        if type(frame.get('score')) is not bool or frame['score'] != (i >= WARMUP):
            raise ValueError(f'frame {i}: temporal scoring must be an 80-frame warmup then 400 scored frames')
        if type(frame.get('reset')) is not bool or frame['reset'] != (i in (0, 360)):
            raise ValueError(f'frame {i}: temporal reset must occur only at ordinals 0 and 360')
        params = frame.get('params', {})
        if not isinstance(params, dict):
            raise ValueError(f'frame {i}: temporal frame_time_ms requires a params object')
        for key, got, expected in (('time_ms', frame.get('time_ms'), i * FRAME_MS),
                                   ('frame_time_ms', params.get('frame_time_ms'), FRAME_MS)):
            # NGX deltas are float32 and moving.json uses nine significant digits.
            if (type(got) not in (int, float) or not math.isfinite(got) or got < 0 or
                    not math.isclose(got, expected, rel_tol=1e-7, abs_tol=1e-5)):
                raise ValueError(f'frame {i}: temporal {key} must match the finite 60 Hz clock')


def corpus_metadata(capture, profile=None):
    if profile is not None:
        validate_temporal_capture(capture, profile)
    frames = capture['frames']
    # Nominal profile time is exact at 60 Hz; the recorded NGX float32 delta is retained per frame.
    deltas = [FRAME_MS if profile is not None else _frame_delta(frames, i) for i in range(len(frames))]
    result = dict(schema='retoque-replay-corpus-summary', version=1, profile=profile or 'legacy',
                  source=capture.get('source'), frames=len(frames),
                  scored_frames=sum(f['score'] for f in frames),
                  nominal_duration_ms=sum(deltas),
                  timestamp_span_ms=frames[-1]['time_ms'] - frames[0]['time_ms'],
                  scored_duration_ms=sum(dt for dt, f in zip(deltas, frames) if f['score']),
                  reset_frame_ids=[f['id'] for f in frames if f['reset']], phases=[])
    if profile is not None:
        for name, first, stop in PHASES:
            phase = frames[first:stop]
            result['phases'].append(dict(name=name, first_frame_id=phase[0]['id'], last_frame_id=phase[-1]['id'],
                                         frames=len(phase), scored_frames=sum(f['score'] for f in phase),
                                         nominal_duration_ms=sum(deltas[first:stop])))
        result['limitations'] = [
            'Synthetic opaque scene rendered through DLAA; no game-content or perceptual acceptance claim',
            'Projection metadata checks history transport; raw motion pixels need the existing GPU guide oracle',
            'Fixed resolution and exposure; no HDR transitions, exposure changes or DRS coverage',
        ]
    return result


def reset_gates(capture, runs):
    """A scored camera cut must run the model, including fixed/adaptive cadence candidates."""
    resets = [i for i, f in enumerate(capture['frames']) if f['score'] and f['reset']]
    results = {}
    for name, run in runs.items():
        if run['settings'].get('Enabled') is False:
            continue
        bad = [run['frames'][i]['id'] for i in resets
               if run['frames'][i]['kind'] not in ('every', 'model') or run['frames'][i]['model_result'] != 1 or
               run['frames'][i]['completed_passes'] < 1 or
               run['frames'][i]['completed_passes'] != run['frames'][i]['requested_passes']]
        results[f'{name}-scored-reset-runs-model'] = dict(
            ok=bool(resets) and not bad, detail=f'{len(resets)} scored reset(s), bad frames {bad[:8]}')
    return results


def phase_observations(capture, runs, profile=PROFILE):
    """Report actual admission on every phase, including warmup and any skipped cut."""
    _validate_capture_identity(capture, profile)
    result = {}
    ids = [f['id'] for f in capture['frames']]
    for name, run in runs.items():
        if [f['id'] for f in run['frames']] != ids:
            raise ValueError(f'{name}: phase observation frame identity mismatch')
        phases = {}
        for phase, first, stop in PHASES:
            counts = dict(model=0, carried=0, skipped=0)
            intervals = {}
            for entry in run['frames'][first:stop]:
                counts['model' if entry['kind'] in ('every', 'model') else entry['kind']] += 1
                interval = str(entry['interval'])
                intervals[interval] = intervals.get(interval, 0) + 1
            phases[phase] = dict(counts=counts, intervals=intervals)
        result[name] = phases
    return result


def assessments(gates, judge):
    verdicts = [v.get('verdict') for v in judge.get('verdicts', {}).values()]
    if not verdicts:
        numerical = 'NOT_RUN'
    elif 'INVALID' in verdicts or None in verdicts:
        numerical = 'INVALID'
    elif 'FAIL' in verdicts:
        numerical = 'FAIL'
    elif any(v != 'PASS' for v in verdicts) or judge.get('calibration', {}).get('verdict') != 'CALIBRATED':
        numerical = 'INCONCLUSIVE'
    else:
        numerical = 'PASS'
    return dict(functional='FAIL' if any(r['ok'] is False for r in gates.values()) else 'PASS',
                numerical=numerical, perceptual='NOT_ASSESSED',
                interpretation='Functional gates and numerical A/A comparison are separate; neither is perceptual approval')
