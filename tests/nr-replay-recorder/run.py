#!/usr/bin/env python3
"""The replay recorder's pure half (docs/dlssnr/design/offline-replay-recorder.md): session state machine against a
scripted backend, refusals, and a capture manifest that parses as schema v2."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'no C++ compiler'
with tempfile.TemporaryDirectory() as scratch:
    exe = Path(scratch) / 'recorder'
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'retoque'), str(HERE / 'cases.cpp'), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], cwd=scratch, capture_output=True, text=True)
    print(run.stdout, end='')
    print(run.stderr, end='')
    if run.returncode:
        raise SystemExit(f'recorder cases failed ({run.returncode})')
    manifest = json.loads((Path(scratch) / 'manifest.json').read_text())
assert manifest['schema'] == 'retoque-replay-capture' and manifest['version'] == 2 and manifest['complete'] is True
assert manifest['source'] == 'game:Cyber"punk' and manifest['provenance']['settings'] == 'a\nb'
frames = manifest['frames']
assert [f['id'] for f in frames] == [1, 2, 3, 4]
assert [f['score'] for f in frames] == [False, False, True, True]  # 0, 1000, 2000, 3000 ms against 1500 ms warmup
assert frames[0]['reset'] is True and not any(f['reset'] for f in frames[1:])
assert frames[0]['color'] == {'path': 'f1.color', 'encoding': 'raw-dxgi', 'dxgi_format': 10, 'width': 64,
                              'height': 32, 'pixel_bytes': 8, 'row_pitch': 512, 'sha256': None}
assert manifest['finalized'] is False  # hashes come from tools/replay/finalize.py, off the game's clock
assert frames[0]['params']['create_flags'] == 3 and frames[0]['params']['mv_scale'] == [1, 1]
assert manifest['color_space'] == 'linear-rec709' and manifest['motion']['includes_jitter'] is False
print('PASS: the manifest parses as capture schema v2 with warmup, reset, planes and params')
