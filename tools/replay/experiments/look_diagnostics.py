"""Run existing debug views on a supplied synthetic replay, under its normal lease.

No compilation, game capture or changes to production configuration. The supplied
DLL needs its companion nvngx.dll_dlssnr.dll sibling. Outputs must be a new directory.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from tools.replay.format import digest, load_capture, need


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('capture', 'dll', 'harness', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    load_capture(args.capture)
    need(not args.out.exists(), 'choose a fresh output directory')
    need(args.dll.with_name('nvngx.dll_dlssnr.dll').is_file(), 'forwarder sibling required')
    need(args.harness.is_file() and args.dll.is_file(), 'binaries required; this script never builds')
    os.environ.update(
        LOOPBACK_REPLAY_CAPTURE=str(args.capture.resolve()),
        LOOPBACK_REPLAY_CALIBRATE='0', LOOPBACK_REPLAY_JUDGE='0',
        LOOPBACK_REPLAY_VARIANTS=json.dumps({
            'proxy': {'Cadence': '1', 'DebugView': '1'},
            'every-a': {'Cadence': '1', 'DebugView': '2'},
            'every-b': {'Cadence': '1', 'DebugView': '2'}}))
    # Reject accidental inherited options that could alter the intended invocation.
    allowed = {'LOOPBACK_REPLAY_CAPTURE', 'LOOPBACK_REPLAY_CALIBRATE',
               'LOOPBACK_REPLAY_JUDGE', 'LOOPBACK_REPLAY_VARIANTS'}
    need(not any(k.startswith('LOOPBACK_REPLAY_') and k not in allowed for k in os.environ),
         'unset other LOOPBACK_REPLAY options')
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tests/dlssnr-loopback'))
    import run
    from run_moving import lease

    run.OUT = args.out.resolve()
    run.RUN = run.OUT / 'replay-run'
    with lease(run.RUN):
        run.RUN.mkdir(parents=True)
        shutil.copy2(args.harness, run.RUN / 'dlssnr-loopback.exe')
        (run.OUT / 'binary-provenance.json').write_text(json.dumps(dict(
            dll_sha256=digest(args.dll), harness_sha256=digest(args.harness),
            forwarder_sha256=digest(args.dll.with_name('nvngx.dll_dlssnr.dll')),
            capture_sha256=digest(args.capture),
            note='Reused binaries; producer.commit in run.json identifies orchestration, not binary build.'), indent=2) + '\n')
        sys.argv = ['run.py', '--replay-nr', '--skip-build', '--dll', str(args.dll.resolve())]
        with run.result_run(run.OUT / 'interruption-result.json'):
            run.main(run.parse_args())


if __name__ == '__main__':
    main()
