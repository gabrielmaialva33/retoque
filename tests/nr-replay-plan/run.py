#!/usr/bin/env python3
"""The replay runner's capture loading and plan v3, without Wine or a GPU (tests/dlssnr-loopback/test_replay_plan.py)."""
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.executable, Path(__file__).resolve().parents[1] / 'dlssnr-loopback/test_replay_plan.py'],
               check=True)
