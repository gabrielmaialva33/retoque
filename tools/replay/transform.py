"""python3 -m tools.replay.transform: metamorphic captures for the replay seam (Codex N-03, W3-B01).

Each subcommand writes a new schema-v2 capture that describes the same scene as its source in another
representation, so a replay of both through tests/dlssnr-loopback --replay-nr can be compared without judging the
model: an exact transformation must give the same decisions and the same output bytes.

  pad      depth and motion re-packed at a non-zero base inside larger planes whose margin is a sentinel; optionally
           the vectors x K with the MV scale / K (K a power of two, checked exact), and MVLowRes set or cleared
           where render and display extents agree. Exact.
  colour8  the colour quantised once to UNORM8 (an optional per-channel gain keeps R far from B, and alpha can be a
           pattern), then written as R8G8B8A8_UNORM (28), B8G8R8A8_UNORM (87) or alternating between them on a
           schedule, the bytes reordered for each frame's format; resets can be placed on swap frames. The all-RGBA8
           and all-BGRA8 captures replay through today's seam; an alternating one does not (see README).
  compare  two replay run directories frame by frame: decisions and output bytes, each output read in its own
           channel order and compared as RGBA.

Stdlib only. Reads v1 (the loopback corpus) and v2 captures; writes v2, finalized, with its own digests.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys

ALLOW_UNORDERED_ACCESS = 0x4
MV_LOW_RES = 1 << 1
IS_HDR = 1 << 0
V1_FORMATS = {'color': 10, 'depth': 41, 'motion': 34}
PIXEL_BYTES = {10: 8, 2: 16, 16: 8, 28: 4, 87: 4, 41: 4, 34: 4}
BGRA = {87, 90, 91}  # B8G8R8A8 UNORM, TYPELESS, UNORM_SRGB: bytes 0 and 2 swapped against their RGBA twins
TWIN = {28: 87, 87: 28}
SENTINELS = {
    # format: {kind: pixel bytes}
    41: {'extreme': struct.pack('<f', 3.0e38), 'nan': struct.pack('<I', 0x7FC00000)},
    34: {'extreme': struct.pack('<HH', 0x7BFF, 0x7BFF), 'nan': struct.pack('<HH', 0x7E00, 0x7E00)},
    16: {'extreme': struct.pack('<ff', 3.0e38, 3.0e38), 'nan': struct.pack('<II', 0x7FC00000, 0x7FC00000)},
}
MOTION_COMPONENT = {34: ('e', 2), 16: ('f', 4)}

try:  # optional, for full-size captures; both paths give the same bytes (tests/nr-replay-transform)
    import numpy
except ImportError:
    numpy = None
BACKEND = os.environ.get('RETOQUE_REPLAY_BACKEND', 'auto')


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(path):
    path = Path(path).expanduser().resolve()
    capture = json.loads(path.read_text())
    if capture.get('schema') != 'retoque-replay-capture' or capture.get('version') not in (1, 2) or \
            not capture.get('complete'):
        raise SystemExit(f'{path}: not a complete retoque-replay-capture v1 or v2')
    if capture['version'] == 2 and capture.get('finalized') is not True:
        raise SystemExit(f'{path}: not finalized')
    return path, capture


def plane_format(capture, key, p):
    return V1_FORMATS[key] if capture['version'] == 1 else p['dxgi_format']


def geometry(capture, f):
    """The frame's geometry as run_replay.py reads it (a v1 frame is full-extent, unscaled)."""
    params = f.get('params', {})
    if capture['version'] == 1:
        w, h = f['color']['width'], f['color']['height']
        return dict(create_flags=params.get('create_flags', 65), ray_reconstruction=False, mv_scale=[1.0, 1.0],
                    frame_time_ms=params.get('frame_time_ms'), render_subrect=[w, h], depth_base=[0, 0],
                    motion_base=[0, 0])
    return dict(create_flags=params['create_flags'], ray_reconstruction=params['ray_reconstruction'],
                mv_scale=list(params['mv_scale']), frame_time_ms=params.get('frame_time_ms'),
                render_subrect=list(params['render_subrect']), depth_base=list(params['depth_base']),
                motion_base=list(params['motion_base']))


def region(capture, f, key):
    """(base x, base y, width, height) of a guide's valid picture inside its plane; refused when it does not fit."""
    g = geometry(capture, f)
    p = f[key]
    rw, rh = g['render_subrect']
    if not rw or not rh:
        raise SystemExit(f'frame {f["id"]}: no render subrect, so no valid region to move')
    if key == 'motion' and not g['create_flags'] & MV_LOW_RES:
        rw, rh = f['color']['width'], f['color']['height']
    bx, by = g['depth_base' if key == 'depth' else 'motion_base']
    if bx + rw > p['width'] or by + rh > p['height']:
        raise SystemExit(f'frame {f["id"]} {key}: the valid region {rw}x{rh} at ({bx},{by}) leaves its '
                         f'{p["width"]}x{p["height"]} plane; a clipped guide has no equivalent representation')
    return bx, by, rw, rh


def frame_record(capture, f, planes, params):
    return dict(id=f['id'], present=f['present'], time_ms=f['time_ms'], reset=f['reset'], score=f['score'],
                jitter=f['jitter'], pre_exposure=f['pre_exposure'], exposure=None,
                exposure_value=f.get('exposure_value', 1), completion='fence-complete', params=params, **planes)


def raw_plane(directory, name, fmt, width, height, source_flags):
    pixel = PIXEL_BYTES[fmt]
    path = directory / name
    if path.stat().st_size != width * height * pixel:
        raise AssertionError(f'{path}: wrong size')
    return dict(path=name, encoding='raw-dxgi', dxgi_format=fmt, width=width, height=height, pixel_bytes=pixel,
                row_pitch=width * pixel, source_format=fmt, source_flags=source_flags, sha256=digest(path))


def link_or_copy(source, target):
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def write_capture(source_path, capture, directory, frames, note, transformation):
    out = dict(schema='retoque-replay-capture', version=2, complete=True, finalized=True, mode='nr-boundary',
               source='derived:' + digest(source_path), clock='recorded-evaluation',
               color_space=capture.get('color_space', 'linear-rec709'), color_jitter='removed',
               depth=capture.get('depth', {}),
               motion=dict(direction='current-to-previous', units='pixels', scale='per-frame', includes_jitter=False),
               regions=capture.get('regions', {}),
               provenance=dict(capture.get('provenance', {}), derived_from=str(source_path), note=note,
                               transformation=transformation),
               frames=frames)
    path = directory / 'capture.json'
    path.write_text(json.dumps(out, indent=1) + '\n')
    (directory / 'transform.json').write_text(json.dumps(transformation, indent=1) + '\n')
    return path


def fresh(directory):
    directory = Path(directory).expanduser().resolve()
    if directory.exists() and any(directory.iterdir()):
        raise SystemExit(f'{directory}: not empty; a derived capture is written into a new directory')
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def colour_plane(capture, f, base, directory):
    """The source colour, linked, in its own format (a v1 colour is R16G16B16A16_FLOAT)."""
    p = f['color']
    name = f'f{f["id"]}.color'
    link_or_copy(base / p['path'], directory / name)
    fmt = plane_format(capture, 'color', p)
    flags = ALLOW_UNORDERED_ACCESS if capture['version'] == 1 else p.get('source_flags', ALLOW_UNORDERED_ACCESS)
    plane = raw_plane(directory, name, fmt, p['width'], p['height'], flags)
    plane['source_format'] = fmt if capture['version'] == 1 else p.get('source_format', fmt)
    return plane


# ---------------------------------------------------------------------------------------------------------------------
# pad
# ---------------------------------------------------------------------------------------------------------------------
def fast():
    if BACKEND == 'python':
        return False
    if BACKEND == 'numpy' and numpy is None:
        raise SystemExit('RETOQUE_REPLAY_BACKEND=numpy but NumPy is absent')
    return numpy is not None


def scale_motion(data, fmt, units):
    """Every component x units, refused unless dividing back gives the same bits (a power of two, no overflow or
    precision lost in a subnormal)."""
    code, size = MOTION_COMPONENT[fmt]
    if fast():
        dtype, bits = ('<f2', '<u2') if code == 'e' else ('<f4', '<u4')
        values = numpy.frombuffer(data, dtype=dtype)
        with numpy.errstate(over='ignore', invalid='ignore', under='ignore'):
            scaled = (values * numpy.array(units, dtype=dtype)).astype(dtype)
            back = (scaled / numpy.array(units, dtype=dtype)).astype(dtype)
        if not numpy.all(numpy.isfinite(scaled) | ~numpy.isfinite(values)) or \
                not numpy.array_equal(back.view(bits), values.view(bits)):
            raise SystemExit(f'a motion component x {units} is not exact in its format; pick another unit')
        return scaled.tobytes()
    count = len(data) // size
    values = struct.unpack(f'<{count}{code}', data)
    scaled = []
    for v in values:
        s = v * units
        packed = struct.pack(f'<{code}', s) if math.isfinite(s) and abs(s) < (65504 if code == 'e' else 3e38) else None
        if packed is None or struct.unpack(f'<{code}', packed)[0] / units != v or \
                struct.pack(f'<{code}', struct.unpack(f'<{code}', packed)[0] / units) != struct.pack(f'<{code}', v):
            raise SystemExit(f'motion component {v!r} x {units} is not exact in its format; pick another unit')
        scaled.append(struct.unpack(f'<{code}', packed)[0])
    return struct.pack(f'<{count}{code}', *scaled)


def repack(data, width, height, pixel, src, dst_size, dst_base, sentinel):
    """The region src=(bx, by, w, h) of a width x height plane, at dst_base inside a dst_size plane of sentinels."""
    bx, by, rw, rh = src
    gw, gh = dst_size
    dx, dy = dst_base
    out = bytearray(sentinel * (gw * gh))
    for y in range(rh):
        row = data[((by + y) * width + bx) * pixel:((by + y) * width + bx + rw) * pixel]
        start = ((dy + y) * gw + dx) * pixel
        out[start:start + rw * pixel] = row
    return bytes(out)


def pad(args):
    source_path, capture = load(args.capture)
    base = source_path.parent
    directory = fresh(args.out)
    units = float(args.units)
    if units == 0 or not math.isfinite(units) or math.frexp(abs(units))[0] != 0.5:
        raise SystemExit(f'--units {args.units}: a power of two (or its negative), so the transformation is exact')
    bases = dict(depth=tuple(args.depth_base), motion=tuple(args.motion_base))
    frames = []
    for f in capture['frames']:
        g = geometry(capture, f)
        planes = dict(color=colour_plane(capture, f, base, directory))
        flags = g['create_flags']
        if args.mv_low_res != 'keep':
            wanted = flags | MV_LOW_RES if args.mv_low_res == 'on' else flags & ~MV_LOW_RES
            if wanted != flags and tuple(g['render_subrect']) != (f['color']['width'], f['color']['height']):
                raise SystemExit(f'frame {f["id"]}: MVLowRes changes which extent the vectors cover unless render '
                                 'and display agree; not an exact transformation here')
            flags = wanted
        for key in ('depth', 'motion'):
            p = f[key]
            fmt = plane_format(capture, key, p)
            if fmt not in SENTINELS:
                raise SystemExit(f'frame {f["id"]} {key}: format {fmt} has no sentinel defined')
            src = region(capture, f, key)
            data = (base / p['path']).read_bytes()
            pixel = PIXEL_BYTES[fmt]
            if key == 'motion' and units != 1.0:
                data = scale_motion(data, fmt, units)
            size = (bases[key][0] + src[2] + args.pad[0], bases[key][1] + src[3] + args.pad[1])
            name = f'f{f["id"]}.{key}'
            (directory / name).write_bytes(repack(data, p['width'], p['height'], pixel, src, size, bases[key],
                                                  SENTINELS[fmt][args.sentinel]))
            flags_in = 0 if capture['version'] == 1 else p.get('source_flags', 0)
            planes[key] = raw_plane(directory, name, fmt, size[0], size[1], flags_in)
            if capture['version'] == 2:
                planes[key]['source_format'] = p.get('source_format', fmt)
        rw, rh = g['render_subrect']
        params = dict(create_flags=flags, ray_reconstruction=g['ray_reconstruction'],
                      mv_scale=[g['mv_scale'][0] / units, g['mv_scale'][1] / units], frame_time_ms=g['frame_time_ms'],
                      render_subrect=[rw, rh], depth_base=list(bases['depth']), motion_base=list(bases['motion']))
        frames.append(frame_record(capture, f, planes, params))
    transformation = dict(kind='pad', exact=True, depth_base=list(bases['depth']), motion_base=list(bases['motion']),
                          pad=list(args.pad), sentinel=args.sentinel, units=units, mv_low_res=args.mv_low_res)
    path = write_capture(source_path, capture, directory, frames,
                         f'guides at a base in padded planes ({args.sentinel} margin), vectors x {units:g}, '
                         f'MV scale / {units:g}', transformation)
    print(path)
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# colour8
# ---------------------------------------------------------------------------------------------------------------------
def unorm8(v):
    if not math.isfinite(v) or v <= 0.0:
        return 0
    return 255 if v >= 1.0 else int(v * 255.0 + 0.5)


def quantise(data, fmt, width, height, gain, alpha, frame_id):
    """RGBA8 bytes of a float colour plane: each channel x its gain, clamped, rounded once."""
    if fmt in (10, 2) and fast():
        values = numpy.frombuffer(data, dtype='<f2' if fmt == 10 else '<f4').astype(numpy.float64).reshape(-1, 4)
        out = numpy.empty(values.shape, dtype=numpy.uint8)
        with numpy.errstate(invalid='ignore', over='ignore'):
            for c in range(3):
                v = values[:, c] * gain[c]
                q = numpy.floor(v * 255.0 + 0.5)
                q = numpy.where(v >= 1.0, 255.0, q)
                q = numpy.where(~numpy.isfinite(v) | (v <= 0.0), 0.0, q)
                out[:, c] = q.astype(numpy.uint8)
            if alpha == 'source':
                v = values[:, 3]
                q = numpy.where(v >= 1.0, 255.0, numpy.floor(v * 255.0 + 0.5))
                out[:, 3] = numpy.where(~numpy.isfinite(v) | (v <= 0.0), 0.0, q).astype(numpy.uint8)
            else:
                i = numpy.arange(width * height)
                out[:, 3] = ((i % width) * 7 + (i // width) * 13 + frame_id * 29) & 0xFF
        return out.tobytes()
    if fmt == 10:
        values = struct.unpack(f'<{width * height * 4}e', data)
    elif fmt == 2:
        values = struct.unpack(f'<{width * height * 4}f', data)
    else:
        raise SystemExit(f'colour format {fmt}: colour8 quantises float colour (10 or 2) only')
    out = bytearray(width * height * 4)
    for i in range(width * height):
        r, g, b, a = values[4 * i:4 * i + 4]
        out[4 * i] = unorm8(r * gain[0])
        out[4 * i + 1] = unorm8(g * gain[1])
        out[4 * i + 2] = unorm8(b * gain[2])
        if alpha == 'source':
            out[4 * i + 3] = unorm8(a)
        else:
            x, y = i % width, i // width
            out[4 * i + 3] = (x * 7 + y * 13 + frame_id * 29) & 0xFF
    return bytes(out)


def reorder(rgba):
    """RGBA8 <-> BGRA8: bytes 0 and 2 of every pixel swapped (an involution)."""
    out = bytearray(rgba)
    out[0::4], out[2::4] = rgba[2::4], rgba[0::4]
    return bytes(out)


def schedule(frames, every, start):
    """Indices of the frames whose format differs from their predecessor's when alternating every `every` frames from
    index `start`: one swap each `every` frames."""
    swaps = []
    for i in range(len(frames)):
        if i >= start and (i - start) % every == 0 and i > 0:
            swaps.append(i)
    return swaps


def colour8(args):
    source_path, capture = load(args.capture)
    base = source_path.parent
    directory = fresh(args.out)
    gain = tuple(args.gain)
    frames_in = capture['frames']
    swaps = schedule(frames_in, args.swap_every, args.swap_start)
    # BGRA from each swap frame to the next one in the alternating order: format by frame index.
    bgra_at = set()
    on = False
    for i in range(len(frames_in)):
        if i in swaps:
            on = not on
        if on:
            bgra_at.add(i)
    resets = set()
    if args.reset_on_swap:
        scored_swaps = [i for i in swaps if frames_in[i]['score']]
        for k in args.reset_on_swap:
            if k < 1 or k > len(scored_swaps):
                raise SystemExit(f'--reset-on-swap {k}: only {len(scored_swaps)} scored swap frames')
            resets.add(scored_swaps[k - 1])
    frames = []
    asymmetry = []
    for i, f in enumerate(frames_in):
        p = f['color']
        fmt = plane_format(capture, 'color', p)
        rgba = quantise((base / p['path']).read_bytes(), fmt, p['width'], p['height'], gain, args.alpha, f['id'])
        step = max(1, len(rgba) // 4 // 4096) * 4  # a sample of pixels is enough for the report
        asymmetry.append(sum(abs(rgba[j] - rgba[j + 2]) for j in range(0, len(rgba), step)) / len(range(0, len(rgba), step)))
        bgra = args.order == 'bgra' or (args.order == 'alternate' and i in bgra_at)
        out_fmt = 87 if bgra else 28
        name = f'f{f["id"]}.color'
        (directory / name).write_bytes(reorder(rgba) if bgra else rgba)
        planes = dict(color=raw_plane(directory, name, out_fmt, p['width'], p['height'], ALLOW_UNORDERED_ACCESS))
        for key in ('depth', 'motion'):
            q = f[key]
            name = f'f{f["id"]}.{key}'
            link_or_copy(base / q['path'], directory / name)
            qf = plane_format(capture, key, q)
            planes[key] = raw_plane(directory, name, qf, q['width'], q['height'],
                                    0 if capture['version'] == 1 else q.get('source_flags', 0))
        g = geometry(capture, f)
        flags = g['create_flags'] & ~IS_HDR if args.clear_hdr else g['create_flags']
        params = dict(create_flags=flags, ray_reconstruction=g['ray_reconstruction'], mv_scale=g['mv_scale'],
                      frame_time_ms=g['frame_time_ms'], render_subrect=g['render_subrect'],
                      depth_base=g['depth_base'], motion_base=g['motion_base'])
        record = frame_record(capture, f, planes, params)
        record['reset'] = bool(f['reset'] or i in resets)
        frames.append(record)
    transformation = dict(kind='colour8', exact=True, order=args.order, gain=list(gain), alpha=args.alpha,
                          clear_hdr=args.clear_hdr, swap_every=args.swap_every, swap_start=args.swap_start,
                          swap_frames=[frames_in[i]['id'] for i in swaps],
                          bgra_frames=sorted(frames_in[i]['id'] for i in bgra_at) if args.order == 'alternate' else
                          ([f['id'] for f in frames_in] if args.order == 'bgra' else []),
                          reset_frames=sorted(frames_in[i]['id'] for i in resets),
                          mean_abs_r_minus_b=sum(asymmetry) / len(asymmetry),
                          seam_supported=args.order != 'alternate')
    path = write_capture(source_path, capture, directory, frames,
                         f'colour quantised to UNORM8 (gain {gain}), {args.order} channel order', transformation)
    print(path)
    if args.order == 'alternate':
        print('note: run_replay.py refuses a colour format that changes within a capture today (README, "colour8")',
              file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# compare
# ---------------------------------------------------------------------------------------------------------------------
DECISIONS = ('kind', 'interval', 'requested_passes', 'completed_passes', 'model_result', 'skip')


def manifest(directory):
    directory = Path(directory).expanduser().resolve()
    for name in ('run.json', 'control.json'):
        if (directory / name).is_file():
            return directory, json.loads((directory / name).read_text())
    raise SystemExit(f'{directory}: no run.json or control.json')


def canonical_output(directory, out):
    data = (directory / out['path']).read_bytes()
    if digest(directory / out['path']) != out['sha256']:
        raise SystemExit(f'{directory / out["path"]}: digest mismatch')
    fmt = out.get('dxgi_format', 10)
    return reorder(data) if fmt in BGRA else data, 28 if fmt in (28, 87) else fmt


def compare_runs(a_dir, b_dir, scored_only=False):
    a_dir, a = manifest(a_dir)
    b_dir, b = manifest(b_dir)
    if [f['id'] for f in a['frames']] != [f['id'] for f in b['frames']]:
        return dict(ok=False, detail='the runs replayed different frame sequences')
    decisions, pixels, worst = [], [], 0
    for x, y in zip(a['frames'], b['frames']):
        if any(x.get(k) != y.get(k) for k in DECISIONS):
            decisions.append(x['id'])
        ca, fa = canonical_output(a_dir, x['output'])
        cb, fb = canonical_output(b_dir, y['output'])
        if fa != fb or len(ca) != len(cb):
            pixels.append(x['id'])
            continue
        if ca != cb:
            pixels.append(x['id'])
            if fa == 28:
                worst = max(worst, max(abs(p - q) for p, q in zip(ca, cb)))
    return dict(ok=not decisions and not pixels, frames=len(a['frames']), decisions_differ=decisions[:16],
                pixels_differ=pixels[:16], pixels_differ_count=len(pixels), worst_unorm8_step=worst,
                settings_a=a.get('settings'), settings_b=b.get('settings'))


def compare(args):
    result = compare_runs(args.a, args.b)
    print(json.dumps(result, indent=1))
    return 0 if result['ok'] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(prog='python3 -m tools.replay.transform', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pad', help='guides at a base in padded planes, sentinel margin, optional unit change')
    p.add_argument('capture')
    p.add_argument('--out', required=True)
    p.add_argument('--depth-base', type=int, nargs=2, default=[16, 12], metavar=('X', 'Y'))
    p.add_argument('--motion-base', type=int, nargs=2, default=[24, 8], metavar=('X', 'Y'))
    p.add_argument('--pad', type=int, nargs=2, default=[8, 20], metavar=('X', 'Y'), help='margin after the region')
    p.add_argument('--sentinel', choices=('extreme', 'nan'), default='extreme')
    p.add_argument('--units', default='1', help='vectors x UNITS, MV scale / UNITS (power of two)')
    p.add_argument('--mv-low-res', choices=('keep', 'on', 'off'), default='keep')
    p.set_defaults(run=pad)
    c = sub.add_parser('colour8', help='UNORM8 colour as RGBA8, BGRA8 or alternating twins')
    c.add_argument('capture')
    c.add_argument('--out', required=True)
    c.add_argument('--order', choices=('rgba', 'bgra', 'alternate'), required=True)
    c.add_argument('--gain', type=float, nargs=3, default=[8.0, 3.0, 0.5], metavar=('R', 'G', 'B'),
                   help='per-channel gain before quantising; the default lifts the loopback corpus (linear, about 0.03-0.07) so R sits far from B')
    c.add_argument('--alpha', choices=('pattern', 'source'), default='pattern')
    c.add_argument('--clear-hdr', action='store_true', help='drop IsHDR from the create flags')
    c.add_argument('--swap-every', type=int, default=7)
    c.add_argument('--swap-start', type=int, default=0)
    c.add_argument('--reset-on-swap', type=int, nargs='*', default=[],
                   help='set Reset on the K-th scored swap frame (1-based), in every order')
    c.set_defaults(run=colour8)
    m = sub.add_parser('compare', help='two replay runs: decisions and outputs in canonical channel order')
    m.add_argument('a')
    m.add_argument('b')
    m.set_defaults(run=compare)
    args = parser.parse_args(argv)
    if getattr(args, 'swap_every', 1) < 1:
        raise SystemExit('--swap-every must be at least 1')
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
