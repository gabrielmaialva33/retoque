#!/usr/bin/env python3
"""Analyze the ControlMask re-test readbacks; numpy + Pillow, pixels only, no claims from logs.

The 2026-10-30 matrix (README, "ControlMask re-test"): an RGBA8 mask whose R, G and B are reported to scale
intensity, local tone and structure. Every trial creates its own model with Reset on frame 0 and identical
history, so two trials differ only in what they set. Three kinds of comparison:

- a trial against its unmasked twin (a0-null-r0, or a1-null in block B): what the mask changed;
- a mask trial against the global control its channel is reported to scale (a0-int50, a0-tone0,
  a0-struct0): whether the mask does what that control does;
- a trial's output against its own source frame (the `before` capture): whether a mask restored the input.

a0-null-r0 against a0-null-r1 is the repeat floor. If it is not 0/0 nothing else here can be read at face value.
Every comparison is split by the HUD cover the harness saved. The acceptance verdicts are printed and written
to mask-report.json; they are a reading of this run, not a pass/fail of the harness."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

FRAMES = (1, 8, 15, 16, 24, 31)
TRIALS = (
    'a0-r0-nosubrect', 'a0-null-r0', 'a0-null-r1', 'a0-r8-zeros', 'a0-ones', 'a0-zeros', 'a0-r0', 'a0-r128',
    'a0-r0-hud', 'a0-g0', 'a0-b0', 'a0-int50', 'a0-tone0', 'a0-struct0', 'a1-null',
    'a1-ones-m0', 'a1-ones-m1', 'a1-ones-m2', 'a1-ones-m3', 'a1-b0-m0', 'a1-b0-m1', 'a1-b0-m2', 'a1-b0-m3',
    'a0-null-after',
)
PAIRS = (
    # Floors and sanity.
    ('a0-null-r0', 'a0-null-r1', 'repeat floor: two unmasked runs'),
    ('a0-null-r0', 'a0-null-after', 'null after every mask and blend mode'),
    ('a0-null-r0', 'a1-null', 'UseAutoMask 0 vs 1, no mask'),
    # Block A: each mask against the unmasked twin (UseAutoMask 0).
    ('a0-null-r0', 'a0-r0-nosubrect', 'R=0, subrect never written'),
    ('a0-null-r0', 'a0-r8-zeros', 'R8 zeros (2026-09-27 repeat)'),
    ('a0-null-r0', 'a0-ones', 'RGBA8 ones'),
    ('a0-null-r0', 'a0-zeros', 'RGBA8 zeros'),
    ('a0-null-r0', 'a0-r0', 'R=0'),
    ('a0-null-r0', 'a0-r128', 'R=128'),
    ('a0-null-r0', 'a0-r0-hud', 'R=0 inside the HUD only'),
    ('a0-null-r0', 'a0-g0', 'G=0'),
    ('a0-null-r0', 'a0-b0', 'B=0'),
    # The global controls must move the picture, or comparing a mask with them says nothing.
    ('a0-null-r0', 'a0-int50', 'control: Intensity 0.5'),
    ('a0-null-r0', 'a0-tone0', 'control: LocalTone 0'),
    ('a0-null-r0', 'a0-struct0', 'control: LocalStructure 0'),
    # Each channel against the control it is reported to scale.
    ('a0-int50', 'a0-r128', 'R=128 vs Intensity 0.5'),
    ('a0-tone0', 'a0-g0', 'G=0 vs LocalTone 0'),
    ('a0-struct0', 'a0-b0', 'B=0 vs LocalStructure 0'),
    ('a0-r0', 'a0-r0-nosubrect', 'R=0: subrect written vs not'),
    ('a0-zeros', 'a0-r8-zeros', 'zeros: RGBA8 vs R8'),
    # Block B: UseAutoMask 1 with MaskBlendMode 0-3. Against a1-null: what the mask changed with the auto
    # mask on. Against the block A trial with the same mask: whether the auto mask survived (310.8 is
    # reported to drop it whenever any mask is set; 310.9.16 mode 0 is read to drop it too).
    *((('a1-null', f'a1-ones-m{m}', f'ones, MaskBlendMode {m} (auto on)')) for m in range(4)),
    *((('a0-ones', f'a1-ones-m{m}', f'ones, auto on m{m} vs auto off')) for m in range(4)),
    *((('a1-null', f'a1-b0-m{m}', f'B=0, MaskBlendMode {m} (auto on)')) for m in range(4)),
    *((('a0-b0', f'a1-b0-m{m}', f'B=0, auto on m{m} vs auto off')) for m in range(4)),
)
# "Approximately the control": the mask trial is closer to the control than this fraction of how far the
# control moved the picture from the unmasked twin, at every frame and in both regions. A harness choice,
# stated so the verdict can be re-read with another one from the JSON.
NEAR = 0.25
WIDTH, HEIGHT = 1280, 720


def read(path, hashes):
    header = f'P6\n{WIDTH} {HEIGHT}\n255\n'.encode()
    raw = path.read_bytes()
    if not raw.startswith(header) or len(raw) != len(header) + WIDTH * HEIGHT * 3:
        raise RuntimeError(f'incomplete/invalid capture {path}')
    hashes[path.name] = hashlib.sha256(raw).hexdigest()
    return np.frombuffer(raw[len(header):], dtype=np.uint8).reshape(HEIGHT, WIDTH, 3)


def metric(a, b, where):
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))[where]
    if delta.size == 0:
        return {'mae': 0.0, 'max': 0, 'changed': 0.0}
    return {'mae': round(float(delta.mean()), 4), 'max': int(delta.max()),
            'changed': round(float(np.count_nonzero(delta)) / delta.size, 4)}


def compare(x, y, regions):
    return {name: metric(x, y, where) for name, where in regions.items()}


def worst(frames, region, key):
    return max(frames[f][region][key] for f in frames)


def verdicts(pairs, source):
    def pair(a, b):
        return pairs[(a, b)]

    def unmasked_differs():
        return worst(source['a0-null-r0'], 'hud', 'max') > 1 and worst(source['a0-null-r0'], 'rest', 'max') > 1

    floor = pair('a0-null-r0', 'a0-null-r1')
    floor_max = max(worst(floor, 'hud', 'max'), worst(floor, 'rest', 'max'))
    out = {'repeat_floor': {
        'criterion': 'a0-null-r0 vs a0-null-r1 is 0/0 in both regions at every frame',
        'measured_max': floor_max,
        'met': floor_max == 0 and max(worst(floor, 'hud', 'mae'), worst(floor, 'rest', 'mae')) == 0}}
    r0 = source['a0-r0']
    out['r0_restores_source'] = {
        'criterion': 'a0-r0 equals its source within 1 level everywhere, while a0-null-r0 differs from its source',
        'measured_max': max(worst(r0, 'hud', 'max'), worst(r0, 'rest', 'max')),
        'unmasked_source_max': max(worst(source['a0-null-r0'], 'hud', 'max'),
                                   worst(source['a0-null-r0'], 'rest', 'max')),
        'met': worst(r0, 'hud', 'max') <= 1 and worst(r0, 'rest', 'max') <= 1 and unmasked_differs()}
    hud_only = source['a0-r0-hud']
    rest_vs_null = pair('a0-null-r0', 'a0-r0-hud')
    out['r0_hud_only'] = {
        'criterion': 'a0-r0-hud equals its source within 1 level inside the HUD, and outside it differs '
                     'from a0-null-r0 by no more than the repeat floor',
        'hud_source_max': worst(hud_only, 'hud', 'max'),
        'rest_vs_unmasked_max': worst(rest_vs_null, 'rest', 'max'),
        'rest_vs_unmasked_mae': worst(rest_vs_null, 'rest', 'mae'),
        'met': worst(hud_only, 'hud', 'max') <= 1 and worst(rest_vs_null, 'rest', 'max') <= floor_max
               and unmasked_differs()}
    for key, mask, control, label in (('r128_is_intensity_half', 'a0-r128', 'a0-int50', 'Intensity 0.5'),
                                      ('b0_is_local_structure_0', 'a0-b0', 'a0-struct0', 'LocalStructure 0'),
                                      ('g0_is_local_tone_0', 'a0-g0', 'a0-tone0', 'LocalTone 0 (informational)')):
        near = pair(control, mask)
        moved = pair('a0-null-r0', control)
        ok = True
        ratios = []
        for frame in near:
            for region in ('hud', 'rest'):
                span = moved[frame][region]['mae']
                gap = near[frame][region]['mae']
                ratios.append(None if span == 0 else round(gap / span, 4))
                ok = ok and span > 0 and gap <= NEAR * span
        out[key] = {'criterion': f'{mask} is within {NEAR:.0%} of how far {label} moved the picture from '
                                 f'a0-null-r0 (MAE, both regions, every frame), and {control} moved it at all',
                    'gap_over_span': ratios, 'met': ok}
    return out


def main(directory):
    root = Path(directory)
    hashes = {}
    cover = read(root / 'mask-hud-cover.ppm', hashes)[..., 0] > 127
    regions = {'hud': cover, 'rest': ~cover}
    report = {'hud_pixels': int(cover.sum()), 'rest_pixels': int((~cover).sum()), 'near_fraction': NEAR}
    provenance = root / 'mask-model.json'
    if provenance.exists():
        report['model'] = json.loads(provenance.read_text())
        model = report['model']
        print(f'model: {Path(model["model"]).name} {model["bytes"]} bytes sha256 {model["sha256"]} '
              f'preset {model["preset"]}')
    else:
        print('model: mask-model.json missing; the report cannot say which model it measured')
    print(f'HUD cover: {report["hud_pixels"]} px, rest: {report["rest_pixels"]} px')
    # Read on demand: holding all 288 captures decoded would take about 800 MB.
    def image(t, f, side):
        return read(root / f'{t}-f{f}-{side}.ppm', hashes)

    header = f'{"comparison":<38} {"frame":>5}  {"HUD mae/max/changed":>24}  {"rest mae/max/changed":>24}'

    def show(label, frame, m):
        hud, rest = m['hud'], m['rest']
        print(f'{label:<38} {frame:>5}  {hud["mae"]:>9.4f}/{hud["max"]:>3}/{hud["changed"]:>7.4f}'
              f'  {rest["mae"]:>9.4f}/{rest["max"]:>3}/{rest["changed"]:>7.4f}')

    print('\noutput against its own source frame')
    print(header)
    source = {}
    for t in TRIALS:
        source[t] = {f: compare(image(t, f, 'after'), image(t, f, 'before'), regions) for f in FRAMES}
        for f in FRAMES:
            show(t, f, source[t][f])
    print('\ntrial against trial')
    print(header)
    pairs = {}
    for a, b, label in PAIRS:
        pairs[(a, b)] = {f: compare(image(a, f, 'after'), image(b, f, 'after'), regions) for f in FRAMES}
        for f in FRAMES:
            show(label, f, pairs[(a, b)][f])
    report['source'] = source
    report['pairs'] = [{'a': a, 'b': b, 'label': label, 'frames': pairs[(a, b)]} for a, b, label in PAIRS]
    report['acceptance'] = verdicts(pairs, source)
    print('\nacceptance (a reading of this run; see the README for what each one means)')
    for key, value in report['acceptance'].items():
        print(f'  {key:<26} {"met" if value["met"] else "NOT met"}: {value["criterion"]}')
    report['sha256'] = hashes
    (root / 'mask-report.json').write_text(json.dumps(report, indent=1))
    print(f'report: {root / "mask-report.json"}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent / 'artifacts/mask-run')
