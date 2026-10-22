"""python3 -m tools.replay.hold CAPTURE.json OUT_DIR [--ms 1600]: give a short game clip a held warmup.

A replay scores only frames after the pass's start-up gates (two 500 ms settle gates under tracked recording, then the
model's creation; offline-replay-recorder.md, "Length"), so the recorder marks frames before 1.5 s of the clip as
warmup. A clip shorter than that, which a game at 3440x1440 can force on the recorder by outrunning its writer, has
nothing to score.

This writes a new finalized capture v2 into OUT_DIR. It opens with the clip's first frame held for --ms milliseconds of
logical time at the clip's own median frame interval, with zero motion (a still picture moves nowhere), then the clip
itself with its first frame reset, so the model's history starts on the game's frames and every one of them is scored.
The held frames pass the gates; they are never scored. Planes are linked (reflinked or hard-linked) from the source,
so nothing is copied but the one zero-motion plane, and the source capture is left untouched.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics


def link(source, target):
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def hold(path, out, ms=1600.0):
    path, out = Path(path), Path(out)
    capture = json.loads(path.read_text())
    if capture.get('schema') != 'retoque-replay-capture' or capture.get('version') != 2:
        raise SystemExit(f'{path}: not a capture schema v2')
    if capture.get('finalized') is not True:
        raise SystemExit(f'{path}: not finalized; run python3 -m tools.replay.finalize {path} first')
    frames = capture['frames']
    if len(frames) < 2:
        raise SystemExit(f'{path}: a clip of {len(frames)} frame(s) has no interval to hold at')
    times = [f['time_ms'] for f in frames]
    step = statistics.median(b - a for a, b in zip(times, times[1:]))
    if not step > 0:
        raise SystemExit(f'{path}: frame times do not move forward')
    held = int(-(-ms // step))  # ceil
    first = frames[0]
    if first['present'] <= held:
        raise SystemExit(f'{path}: presents start at {first["present"]}, too early to hold {held} frames before')

    out.mkdir(parents=True, exist_ok=False)
    base = path.parent
    for f in frames:
        for key in ('color', 'depth', 'motion'):
            link(base / f[key]['path'], out / f[key]['path'])
    motion = first['motion']
    zero = out / 'hold.motion'
    zero.write_bytes(bytes(motion['row_pitch'] * motion['height']))
    zero_sha = hashlib.sha256(zero.read_bytes()).hexdigest()

    result = []
    for i in range(held):
        f = json.loads(json.dumps(first))
        f.update(id=i + 1, present=first['present'] - held + i, time_ms=i * step, reset=i == 0, score=False)
        f['motion'] = dict(motion, path='hold.motion', sha256=zero_sha)
        result.append(f)
    for f in frames:
        f = json.loads(json.dumps(f))
        f.update(id=held + f['id'], time_ms=held * step + f['time_ms'] - first['time_ms'], score=True)
        result.append(f)
    result[held]['reset'] = True  # the model's history starts on the game's first frame

    derived = dict(capture, frames=result,
                   provenance=dict(capture['provenance'], derived_from=str(path),
                                   held=dict(frames=held, ms=held * step, step_ms=step)))
    target = out / 'capture.json'
    target.write_text(json.dumps(derived, indent=1) + '\n')
    return target, held, step


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Give a short game clip a held warmup.')
    parser.add_argument('capture')
    parser.add_argument('out')
    parser.add_argument('--ms', type=float, default=1600.0)
    a = parser.parse_args()
    target, held, step = hold(a.capture, a.out, a.ms)
    print(f'{target}: {held} held frames at {step:.2f} ms, then the clip, every clip frame scored')
