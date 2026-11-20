#!/usr/bin/env python3
"""Linux-only repeated metadata cost smoke test of this harness process, never a game."""
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time

if len(sys.argv) != 2 or sys.platform != 'linux':
    raise SystemExit('usage: python3 check-legacy-probe-cost.py <built CLI executable> (Linux)')
cli = sys.argv[1]
samples = []
with tempfile.TemporaryDirectory(prefix='probe-cost-', dir=Path(__file__).resolve().parent) as directory:
    # A known, tiny x64 PE exercises file hashing without touching any installed game.
    pe = bytearray(512)
    pe[:2] = b'MZ'
    pe[60:64] = (64).to_bytes(4, 'little')
    pe[64:68] = b'PE\0\0'
    pe[68:70] = (0x8664).to_bytes(2, 'little')
    pe[70:72] = (1).to_bytes(2, 'little')
    pe[84:86] = (240).to_bytes(2, 'little')
    pe[88:90] = (0x20b).to_bytes(2, 'little')
    pe[196:200] = (16).to_bytes(4, 'little')
    exe = Path(directory) / 'synthetic.exe'
    exe.write_bytes(pe)
    for selection, args in [('synthetic_pe', ['--exe', str(exe)]), ('selected_harness_pid', ['--pid', str(os.getpid())])]:
        for _ in range(5):
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            start = time.monotonic()
            run = subprocess.run([cli, 'legacy-probe', *args, '--json', '--lang', 'en'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            elapsed = time.monotonic() - start
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            report = json.loads(run.stdout)
            assert len(run.stdout) <= 65536
            assert str(exe).encode() not in run.stdout
            assert report['schema'] == 1
            if selection == 'synthetic_pe':
                import hashlib
                assert report['executable']['sha256'] == hashlib.sha256(pe).hexdigest()
                assert report['executable']['architecture'] == 'x64'
            else:
                assert report['process_identity'] == 'observed'
                assert report['process_architecture'] != 'unknown'
                assert report['modules_complete']
            assert not run.stderr
            samples.append({'selection': selection, 'wall_ms': round(elapsed * 1000, 3), 'cpu_ms': round(((after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)) * 1000, 3), 'output_bytes': len(run.stdout), 'inspection_complete': report['inspection_complete'], 'budget_exhausted': 'probe.budget' in report['issues']})
print(json.dumps({'scope': 'Linux harness only; no game, Proton or native Windows qualification', 'peak_child_rss_kib': resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss, 'samples': samples}, indent=2))
