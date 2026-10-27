#!/usr/bin/env python3
"""Host-only rendering orchestration evidence and command controls."""
from pathlib import Path
import subprocess
import sys

if __name__ == '__main__':
    subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', str(Path(__file__).parent),
                    '-p', 'test_*.py'], check=True, timeout=45)
