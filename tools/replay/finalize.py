"""python3 -m tools.replay.finalize CAPTURE.json: hash a recorded clip's planes, off the game's clock.

The DLL's replay recorder (docs/dlssnr/design/offline-replay-recorder.md) writes planes as fast as the game produces
them and leaves every sha256 null with "finalized": false. This checks each plane's byte count against its descriptor,
hashes it, and rewrites the manifest atomically with "finalized": true. A finalized capture is refused, so its
identity (the SHA-256 of capture.json's bytes) never changes once replays reference it.
"""

import hashlib
import json
from pathlib import Path
import sys


def finalize(path):
    path = Path(path)
    capture = json.loads(path.read_text())
    if capture.get('schema') != 'retoque-replay-capture' or capture.get('version') != 2:
        raise SystemExit(f'{path}: not a capture schema v2')
    if capture.get('finalized') is not False:
        raise SystemExit(f'{path}: already finalized (its identity is fixed)')
    base = path.parent
    for frame in capture['frames']:
        for key in ('color', 'depth', 'motion'):
            plane = frame[key]
            file = base / plane['path']
            if Path(plane['path']).is_absolute() or '..' in Path(plane['path']).parts:
                raise SystemExit(f'frame {frame["id"]} {key}: unsafe path')
            expected = plane['width'] * plane['height'] * plane['pixel_bytes']
            if plane['row_pitch'] != plane['width'] * plane['pixel_bytes'] or file.stat().st_size != expected:
                raise SystemExit(f'frame {frame["id"]} {key}: {file.stat().st_size} bytes, expected {expected}')
            with file.open('rb') as stream:
                plane['sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    capture['finalized'] = True
    tmp = path.with_suffix('.json.tmp')
    tmp.write_text(json.dumps(capture, indent=1) + '\n')
    tmp.replace(path)
    return capture


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: python3 -m tools.replay.finalize CAPTURE.json')
    done = finalize(sys.argv[1])
    print(f'finalized {len(done["frames"])} frames: {sys.argv[1]}')
