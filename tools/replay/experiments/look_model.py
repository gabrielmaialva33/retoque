"""Offline, guide-free bilateral affine look model. No production/runtime imports.

Independent NumPy formulation of the statistical model described by RTX40MFG-Unlock
b97deee (MIT, Michael Robles), dlss5.cpp kBoostShader, inspired by Chen et al. 2016.
This omits that implementation's motion-reprojected grid and residual detail carry.
Run from the repository root with: python -m tools.replay.experiments.look_model.
"""

import argparse
from array import array
import hashlib
import json
from pathlib import Path
import time

import numpy as np

from tools.replay import numpy_metrics as metrics
from tools.replay.events import _phases
from tools.replay.format import Image, descriptor, digest, load_capture, load_run, need
from tools.replay.metrics import mean, percentile

LUMA = np.array([0.2126, 0.7152, 0.0722])
VARIANTS = {"look16x8": (16, 8), "look32x8": (32, 8), "look16x16": (16, 16)}


def encode(x):
    x = np.clip(x, 0, 1)
    return np.where(x < 0.0031308, x * 12.92, 1.055 * np.maximum(x, 1e-8) ** (1 / 2.4) - 0.055)


def decode(x):
    # The existing resolve saturates model RGB before this decode, including DebugView 2.
    x = np.clip(x, 0, 1)
    return np.where(x < 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def fit(x, y, cell=16, bands=8, ridge=0.005):
    """Soft luminance splat, spatially blur moments, identity-prior ridge solve.

    Row-vector convention: prediction = x @ A + bias. Float64 arithmetic, unlike
    the donor's float32 GPU reduction. Empty bins retain the identity transform.
    """
    h, w, _ = x.shape
    gh, gw = (h + cell - 1) // cell, (w + cell - 1) // cell
    tile = ((np.arange(h)[:, None] // cell) * gw + np.arange(w)[None, :] // cell).ravel()
    xx, yy = x.reshape(-1, 3), y.reshape(-1, 3)
    z = np.clip(xx @ LUMA, 0, 1) * (bands - 1)
    lo = np.floor(z).astype(int)
    upper = np.minimum(lo + 1, bands - 1)
    t = z - lo
    # One count, three first moments each, nine second moments each (25 total).
    # Full symmetric Cxx here instead of packing six entries; same sufficient statistics.
    features = np.column_stack((np.ones(len(xx)), xx, yy,
                                (xx[:, :, None] * xx[:, None, :]).reshape(-1, 9),
                                (xx[:, :, None] * yy[:, None, :]).reshape(-1, 9)))
    stats = np.zeros((gh * gw * bands, 25))
    for bins, weights in ((lo, 1 - t), (upper, t)):
        index = tile * bands + bins
        for k in range(25):
            stats[:, k] += np.bincount(index, weights * features[:, k], minlength=len(stats))
    stats = stats.reshape(gh, gw, bands, 25)
    # [1,2,1] in x and y, discard outside neighbours (do not replicate edges).
    for axis in (0, 1):
        padded = np.pad(stats, [(1, 1) if i == axis else (0, 0) for i in range(4)])
        n = stats.shape[axis]
        stats = sum(weight * np.take(padded, np.arange(n) + offset, axis=axis)
                    for offset, weight in enumerate((1, 2, 1)))
    weight = stats[..., 0]
    moments = stats / np.maximum(weight[..., None], 1e-30)
    mx, my = moments[..., 1:4], moments[..., 4:7]
    cxx = moments[..., 7:16].reshape(gh, gw, bands, 3, 3) - mx[..., :, None] * mx[..., None, :]
    cxy = moments[..., 16:25].reshape(gh, gw, bands, 3, 3) - mx[..., :, None] * my[..., None, :]
    eye = np.eye(3)
    cxx += ridge * eye
    cxy += ridge * eye
    valid = (weight > 1e-3) & (np.linalg.det(cxx) > 1e-12)
    a = np.broadcast_to(eye, cxx.shape).copy()
    a[valid] = np.linalg.solve(cxx[valid], cxy[valid])
    bias = my - np.einsum('...i,...ij->...j', mx, a)
    bias[~valid] = 0
    return np.concatenate((a, bias[..., None, :]), axis=-2)


def apply(x, grid, cell):
    """Trilinearly slice coefficients at current pixel centres and current luma."""
    h, w, _ = x.shape
    gh, gw, bands = grid.shape[:3]
    gy = np.broadcast_to(np.clip((np.arange(h)[:, None] + 0.5) / cell - 0.5, 0, gh - 1), (h, w))
    gx = np.broadcast_to(np.clip((np.arange(w)[None, :] + 0.5) / cell - 0.5, 0, gw - 1), (h, w))
    gz = np.clip(x @ LUMA, 0, 1) * (bands - 1)
    bases = [np.floor(v).astype(int) for v in (gy, gx, gz)]
    fracs = [v - i for v, i in zip((gy, gx, gz), bases)]
    out = np.zeros_like(x)
    for dy in (0, 1):
        for dx in (0, 1):
            for dz in (0, 1):
                steps = (dy, dx, dz)
                iy, ix, iz = [np.minimum(i + d, maximum - 1)
                              for i, d, maximum in zip(bases, steps, (gh, gw, bands))]
                weight = np.prod([f if d else 1 - f for f, d in zip(fracs, steps)], axis=0)
                coeff = grid[iy, ix, iz]
                value = np.einsum('...i,...ij->...j', x, coeff[..., :3, :]) + coeff[..., 3, :]
                out += value * weight[..., None]
    return out


def image(values):
    h, w, c = values.shape
    return Image(w, h, c, array('f', np.asarray(values, dtype=np.float32).tobytes()))


def read(root, desc):
    """Restricted v1 raw reader; validate descriptor, bytes, hash and finite pixels."""
    path, w, h, c, pitch = descriptor(root, desc)
    need(desc['encoding'] == 'raw' and desc['dtype'] in ('f16', 'f32'), 'experiment needs raw floats')
    raw = path.read_bytes()
    need(len(raw) == pitch * h, 'plane size mismatch')
    need(hashlib.sha256(raw).hexdigest() == desc['sha256'], 'plane checksum mismatch')
    dtype = np.dtype('<f2' if desc['dtype'] == 'f16' else '<f4')
    values = np.ndarray((h, w, c), dtype=dtype, buffer=raw, strides=(pitch, c * dtype.itemsize, dtype.itemsize))
    need(np.isfinite(values).all(), 'nonfinite plane')
    return values.astype(np.float64)


def summarize(rows):
    result = {}
    for method in sorted({r['method'] for r in rows}):
        result[method] = {}
        for phase in ['all'] + sorted({r['phase'] for r in rows}):
            group = [r for r in rows if r['method'] == method and (phase == 'all' or r['phase'] == phase)]
            if not group:
                continue
            result[method][phase] = {}
            for region in group[0]['metrics']:
                entries = [r['metrics'][region] for r in group]
                pulses = [e['paired_pulse_distance'] for e in entries if e['paired_pulse_distance'] is not None]
                result[method][phase][region] = dict(
                    frames=len(entries), golden_mae_mean=mean([e['golden_mae'] for e in entries]),
                    golden_mae_p95=percentile([e['golden_mae'] for e in entries], .95),
                    paired_pulse_distance_p95=percentile(pulses, .95), pulse_pairs=len(pulses),
                    pulse_missing=len(entries) - len(pulses))
    return result


def evaluate(args):
    out = Path(args.out)
    need(not out.exists(), 'output already exists; choose a fresh evidence directory')
    capture = load_capture(args.capture)
    need(capture['version'] == 1 and capture['color_space'] == 'linear-rec709', 'requires v1 linear corpus')
    _, phases = _phases(capture)
    paths = {name: Path(args.runs) / name / 'run.json' for name in ('proxy', 'every-a', 'every-b')}
    runs = {name: load_run(path, capture) for name, path in paths.items()}
    a, b, proxy = (runs[n] for n in ('every-a', 'every-b', 'proxy'))
    need(len({r['producer']['trial'] for r in runs.values()}) == 3, 'distinct fresh trials required')
    for name, r in runs.items():
        need(r['environment'] == a['environment'] and r['producer']['dll_sha256'] == a['producer']['dll_sha256'],
             'runtime mismatch')
        need(r['settings'] == dict(Cadence='1', DebugView='1' if name == 'proxy' else '2', Enabled=True),
             'unexpected diagnostic settings')
        need(all(f['kind'] == 'every' and f['model_result'] == 1 and f['completed_passes'] == 1
                 for f, src in zip(r['frames'], capture['frames']) if src['score']), 'incomplete model coverage')
    out.mkdir(parents=True)
    capture['_pixel_tolerance'] = .01  # Auxiliary bad-pixel metric only, never a quality gate.
    rows, training, unsafe = [], [], []
    grids, previous, last_y = {}, {}, None
    methods = ['every-b', 'hold', *VARIANTS]
    counts = dict(network_oracle=0, predicted=0)
    start = time.monotonic()
    with (out / 'frames.jsonl').open('w') as stream:
        for index, frame in enumerate(capture['frames']):
            # Validate every raw input, including warmup. Do not silently drop planes or hashes.
            original = read(capture['_root'], frame['color'])
            depth = image(read(capture['_root'], frame['depth']))
            motion = image(read(capture['_root'], frame['motion']))
            decoded = {name: read(run['_root'], run['frames'][index]['output'])[..., :3]
                       for name, run in runs.items()}
            if any(run['frames'][index]['kind'] == 'skipped' for run in runs.values()):
                need(not frame['score'], 'scored skip')
                previous, grids, last_y = {}, {}, None
                continue
            x, y = encode(decoded['proxy']), encode(decoded['every-a'])
            source, golden = image(decoded['proxy']), image(decoded['every-a'])
            network = frame['reset'] or frame['id'] % 2 == 0 or not grids
            if frame['reset'] and grids and frame['score']:
                for name, (cell, _) in VARIANTS.items():
                    pred = decode(apply(x, grids[name], cell))
                    unsafe.append(dict(id=frame['id'], method=name,
                                       golden_mae=float(np.abs(pred - decoded['every-a']).mean())))
                unsafe.append(dict(id=frame['id'], method='hold',
                                   golden_mae=float(np.abs(last_y - decoded['every-a']).mean())))
            if network:
                for name, (cell, bands) in VARIANTS.items():
                    grids[name] = fit(x, y, cell, bands)
                    if frame['score']:
                        fitted = decode(apply(x, grids[name], cell))
                        training.append(dict(id=frame['id'], method=name,
                                             golden_mae=float(np.abs(fitted - decoded['every-a']).mean())))
            outputs = {'every-b': decoded['every-b'], 'hold': decoded['every-a'] if network else last_y}
            for name, (cell, _) in VARIANTS.items():
                outputs[name] = decoded['every-a'] if network else decode(apply(x, grids[name], cell))
            measured = {}
            for name in methods:
                measured[name], previous[name] = metrics.frame_metrics(
                    capture, frame, source, depth, motion, image(outputs[name]), golden, previous.get(name))
            if frame['score']:
                counts['network_oracle' if network else 'predicted'] += 1
                for name in methods:
                    for region, values in measured[name].items():
                        pulse, control = values['pulse'], measured['every-b'][region]['pulse']
                        values['paired_pulse_distance'] = None if pulse is None or control is None else abs(pulse - control)
                    row = dict(id=frame['id'], phase=phases[index], method=name,
                               execution='network-oracle' if network else 'predicted', metrics=measured[name])
                    rows.append(row)
                    stream.write(json.dumps(row, allow_nan=False) + '\n')
            last_y = decoded['every-a']
            if (index + 1) % 40 == 0:
                print(f'{index + 1}/{len(capture["frames"])} ({time.monotonic() - start:.1f}s)', flush=True)
    report = dict(schema='retoque-offline-look-experiment', version=1,
                  status='DESCRIPTIVE_NOT_CALIBRATED', domain='decoded clipped model RGB, DebugScale=1',
                  capture_sha256=capture['_sha256'], run_sha256={n: r['_sha256'] for n, r in runs.items()},
                  environment=a['environment'], dll_sha256=a['producer']['dll_sha256'],
                  experiment_sha256=digest(Path(__file__)), numpy_version=np.__version__,
                  counts=counts, variants=VARIANTS, ridge=.005,
                  all_frames=summarize(rows), predicted_frames=summarize([r for r in rows if r['execution'] == 'predicted']),
                  unsafe_cut=unsafe,
                  training_mae={name: mean([r['golden_mae'] for r in training if r['method'] == name]) for name in VARIANTS},
                  wall_seconds=time.monotonic() - start,
                  limitations=['full-rate oracle history', 'diagnostic FP16 quantization and saturating decode',
                               'no production resolve or guide-free network evaluation', 'no GPU performance measurement'])
    (out / 'result.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')


def self_test():
    rng = np.random.default_rng(39)
    x = rng.uniform(.15, .8, (19, 23, 3))
    for cell, bands in ((16, 8), (32, 8), (16, 16)):
        np.testing.assert_allclose(apply(x, fit(x, x, cell, bands), cell), x, atol=1e-12)
    # Independent dense weighted solve, including neighbouring cells and identity regularizer.
    target = x @ np.array([[.7, .2, 0], [0, .8, .1], [.1, 0, .9]]) + .03
    grid = fit(x, target, 8, 8)
    yy, xx = np.indices(x.shape[:2])
    for cy, cx, band in ((0, 0, 3), (1, 1, 4), (2, 2, 5)):
        dy, dx = abs(yy // 8 - cy), abs(xx // 8 - cx)
        weights = np.maximum(0, 2 - dy) * np.maximum(0, 2 - dx)
        weights = weights * np.maximum(0, 1 - abs((x @ LUMA) * 7 - band))
        w = weights.ravel()
        mx = np.average(x.reshape(-1, 3), weights=w, axis=0)
        my = np.average(target.reshape(-1, 3), weights=w, axis=0)
        left = np.vstack(((x.reshape(-1, 3) - mx) * np.sqrt(w / w.sum())[:, None], np.sqrt(.005) * np.eye(3)))
        right = np.vstack(((target.reshape(-1, 3) - my) * np.sqrt(w / w.sum())[:, None], np.sqrt(.005) * np.eye(3)))
        a = np.linalg.lstsq(left, right, rcond=None)[0]
        np.testing.assert_allclose(grid[cy, cx, band, :3], a, atol=1e-12)
        np.testing.assert_allclose(grid[cy, cx, band, 3], my - mx @ a, atol=1e-12)
    black = np.zeros((17, 19, 3))
    np.testing.assert_array_equal(apply(black, fit(black, black), 16), black)
    np.testing.assert_allclose(decode(encode(x)), x, atol=1e-12)
    # Spatial and intensity interpolation must reproduce a constant affine map at borders too.
    grid = np.zeros((2, 2, 8, 4, 3))
    grid[..., :3, :] = np.eye(3) * .8
    grid[..., 3, :] = .1
    np.testing.assert_allclose(apply(x, grid, 16), .8 * x + .1, atol=1e-12)
    grid = rng.normal(size=(3, 4, 8, 4, 3))
    output = apply(x, grid, 8)
    for py, px in ((0, 0), (8, 9), (18, 22)):
        coord = [np.clip((py + .5) / 8 - .5, 0, 2), np.clip((px + .5) / 8 - .5, 0, 3), (x[py, px] @ LUMA) * 7]
        coeff = np.zeros((4, 3))
        for iy in range(3):
            for ix in range(4):
                for iz in range(8):
                    weight = max(0, 1 - abs(coord[0] - iy)) * max(0, 1 - abs(coord[1] - ix)) * max(0, 1 - abs(coord[2] - iz))
                    coeff += weight * grid[iy, ix, iz]
        np.testing.assert_allclose(output[py, px], np.append(x[py, px], 1) @ coeff, atol=1e-12)
    print('PASS: identity (3 variants), dense least-squares oracle (3 cells), black/empty bins, transfer roundtrip, affine slicing, scalar slicing oracle (3 pixels)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--runs', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if not all((args.capture, args.runs, args.out)):
            parser.error('--capture, --runs and --out are required')
        evaluate(args)
