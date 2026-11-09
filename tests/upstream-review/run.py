#!/usr/bin/env python3
"""Regression reproductions for the independent upstream integration review."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
p=argparse.ArgumentParser();p.add_argument('--ref');p.add_argument('--only');args=p.parse_args()
here=Path(__file__).resolve().parent
env=os.environ.copy()
if args.ref:env['RETOQUE_REVIEW_REF']=args.ref
paths=sorted(x for x in here.glob('*.py') if x.name not in {'run.py','support.py'})
if args.only:paths=[x for x in paths if x.stem in args.only.split(',')]
if not paths:raise SystemExit('No cases selected')
failed=[]
for path in paths:
    if subprocess.run([sys.executable,str(path)],env=env).returncode:failed.append(path.stem)
if failed:raise SystemExit('FAIL: '+', '.join(failed))
print(f'PASS: {len(paths)} review case groups')
