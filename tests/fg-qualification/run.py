#!/usr/bin/env python3
"""Portable evidence and supervision controls for the real-provider fixture."""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
if __name__ == "__main__":
    subprocess.run([sys.executable, str(HERE.parent / "fg-provider-d3d12" / "test_qualification.py")],
                   check=True, timeout=45)
